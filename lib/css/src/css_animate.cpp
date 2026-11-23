// How a presentation runs a document's animations: the CSS animations and transitions that its styles name, the
// keyframes of every effect computed for its targets' styles, and the values that the effects apply over those styles.
//
// css-transitions-1 - https://drafts.csswg.org/css-transitions-1/
// css-transitions-2 - https://drafts.csswg.org/css-transitions-2/
// css-animations-1 - https://drafts.csswg.org/css-animations-1/
// css-animations-2 - https://drafts.csswg.org/css-animations-2/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/

#include "css/src/css_animate.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "css/src/css_animation.h"
#include "css/src/css_cascade.h"
#include "css/src/css_keyword.h"
#include "css/src/css_parse.h"
#include "css/src/css_presentation.h"
#include "css/src/css_property.h"
#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// The groups whose properties animate. The animation properties themselves do not.
constexpr CssStyleGroup kAnimatedGroups[] = {
    CssStyleGroup_Font, CssStyleGroup_Color, CssStyleGroup_Box, CssStyleGroup_Flex, CssStyleGroup_Decoration,
    CssStyleGroup_Background, CssStyleGroup_Effect, CssStyleGroup_Compositing, CssStyleGroup_Generated,
    CssStyleGroup_List, CssStyleGroup_Svg, CssStyleGroup_SvgGeometry, CssStyleGroup_Table, CssStyleGroup_Typography,
    CssStyleGroup_Grid, CssStyleGroup_Fragmentation,
};

CssValueRun listRun(const CssValueList& list) {
    return cssValueListRun(&list);
}

// Returns the element that originates `target`: the target itself, or the element of a pseudo-element.
const PrDomElement* originOf(const PrDomNode* target) {
    while (target->type == DomTargetType_PseudoElement)
        target = target->parent;
    return static_cast<const PrDomElement*>(target);
}

// Returns the pseudo-element that `target` is, or CssPseudo_None for an element.
CssPseudo pseudoOf(const PrDomNode* target) {
    if (target->type != DomTargetType_PseudoElement)
        return CssPseudo_None;
    return static_cast<const CssPresentationPseudo*>(target)->pseudo;
}

bool displaysNone(const CssComputedStyle* style) {
    CssValueRun display = cssComputedValue(style, PrCssProperty_Display);
    for (uint32_t i = 0; i < display.count; ++i) {
        if (cssValueIsKeyword(&display.values[i], PrCssKeyword_None))
            return true;
    }
    return false;
}

CssAnimations& animationsOf(const PrCssPresentation* presentation) {
    return cssAnimations(presentation->document);
}

// Returns the effects that target `target`, or null if none does.
const RawArray<PrCssKeyframeEffect*>* PR_NULLABLE effectsOf(const PrCssPresentation* presentation, const PrDomNode* target) {
    const CssAnimations& animations = animationsOf(presentation);
    uint32_t at = cssAnimatedTarget(animations, originOf(target), pseudoOf(target));
    return at != kHashMissing ? &animations.targets.data[at].effects : nullptr;
}

// Returns the effects that target `target` that animations play, in composite order, in `out`.
void stackOf(const PrCssPresentation* presentation, const PrDomNode* target, RawArray<PrCssKeyframeEffect*>& out) {
    out.clear();
    if (const RawArray<PrCssKeyframeEffect*>* effects = effectsOf(presentation, target))
        out.append(effects->data, effects->count);
    sortItems(out.span(), [](PrCssKeyframeEffect* x, PrCssKeyframeEffect* y) {
        return cssAnimationBefore(x->animation, y->animation);
    });
}

// Returns the CSS animation or transition that the presentation runs on `target` for which `match` returns true, or
// null.
template <typename Match>
PrCssAnimation* PR_NULLABLE ownedOf(PrCssPresentation* presentation, const PrDomNode* target, Match match) {
    if (const RawArray<PrCssKeyframeEffect*>* effects = effectsOf(presentation, target)) {
        for (uint32_t i = 0; i < effects->count; ++i) {
            PrCssAnimation* a = effects->data[i]->animation;
            if (a->owner == presentation && match(a))
                return a;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSS events (css-animations-2/#events, css-transitions-2/#transition-events)

// Queues an event of a CSS animation or transition for the next rendering update, if a window dispatches them.
void queueCssEvent(PrCssAnimation* animation, PrStringView type, double elapsedMilliseconds, double scheduled) {
    CssAnimations& animations = cssAnimations(animation->document);
    if (!animations.recordsEvents || animation->effect == nullptr || animation->effect->target == nullptr)
        return;
    PrDomElement* element = animation->effect->target;
    domNodeHold(element);
    prCssAnimationRetain(animation);
    bool transition = animation->kind == PrCssAnimationKind_Transition;
    animations.events.push(CssAnimateEvent{
        .target = element,
        .animation = animation,
        .type = type,
        .kind = PrDomEventKind(transition ? PrDomEventKind_Transition : PrDomEventKind_Animation),
        .pseudo = animation->effect->pseudo,
        .name = prRawStringCopy(prCssAnimationName(animation)),
        .elapsedTime = elapsedMilliseconds / 1000,
        .currentTime = kPrF64Nan,
        .timelineTime = kPrF64Nan,
        .scheduled = scheduled,
    });
}

// The phase that the event tables compare: idle without a current time, and otherwise the effect's.
AnimationPhase eventPhaseOf(const PrCssAnimation* animation) {
    if (animation->effect == nullptr)
        return AnimationPhase_Idle;
    switch (cssAnimationComputedTiming(animation).phase) {
        case PrLayoutEffectPhase_Before: return AnimationPhase_Before;
        case PrLayoutEffectPhase_Active: return AnimationPhase_Active;
        case PrLayoutEffectPhase_After: return AnimationPhase_After;
        default: return AnimationPhase_Idle;
    }
}

// The start and end of the active interval that elapsed times report, clamped to the active duration.
double intervalStart(const PrLayoutEffectTiming& t, double active) {
    return prF64Max(prF64Min(-t.delay, active), 0);
}

double intervalEnd(const PrLayoutEffectTiming& t, double active) {
    return prF64Max(prF64Min(t.endDelay + active, active), 0);
}

// The time on the document's timeline at which the animation's time is `local`, or NaN.
double timelineAt(const PrCssAnimation* animation, double local) {
    if (prF64IsNan(animation->playback.startTime) || animation->playback.rate == 0 || animation->timeline == nullptr)
        return kPrF64Nan;
    return animation->playback.startTime + local / animation->playback.rate + animation->timeline->originTime;
}

// Queues the events of a CSS animation or transition whose phase or iteration changed since its events last ran.
void findCssEvents(PrCssAnimation* animation) {
    if (animation->owner == nullptr || animation->effect == nullptr)
        return;
    AnimationPhase now = eventPhaseOf(animation);
    AnimationPhase was = animation->eventPhase;
    double iteration = now == AnimationPhase_Active ? cssAnimationComputedTiming(animation).currentIteration : kPrF64Nan;
    if (now == was && (now != AnimationPhase_Active || iteration == animation->eventIteration))
        return;
    const PrLayoutEffectTiming& t = animation->effect->timing;
    PrLayoutComputedTiming computed = cssAnimationComputedTiming(animation);
    double active = computed.activeDuration;
    double from = intervalStart(t, active);
    double to = intervalEnd(t, active);
    double start = timelineAt(animation, t.delay + from);
    double end = timelineAt(animation, t.delay + to);
    if (animation->kind == PrCssAnimationKind_Transition) {
        if (was == AnimationPhase_Idle) {
            queueCssEvent(animation, "transitionrun", from, timelineAt(animation, 0));
            if (now == AnimationPhase_Active || now == AnimationPhase_After)
                queueCssEvent(animation, "transitionstart", from, start);
            if (now == AnimationPhase_After)
                queueCssEvent(animation, "transitionend", to, end);
        } else if (was == AnimationPhase_Before) {
            if (now == AnimationPhase_Active || now == AnimationPhase_After)
                queueCssEvent(animation, "transitionstart", from, start);
            if (now == AnimationPhase_After)
                queueCssEvent(animation, "transitionend", to, end);
        } else if (was == AnimationPhase_Active) {
            if (now == AnimationPhase_After)
                queueCssEvent(animation, "transitionend", to, end);
            else if (now == AnimationPhase_Before)
                queueCssEvent(animation, "transitionend", from, start);
        } else if (was == AnimationPhase_After) {
            if (now == AnimationPhase_Active)
                queueCssEvent(animation, "transitionstart", to, end);
            else if (now == AnimationPhase_Before) {
                queueCssEvent(animation, "transitionstart", to, end);
                queueCssEvent(animation, "transitionend", from, start);
            }
        }
    } else {
        double duration = t.duration;
        bool forwards = animation->playback.rate >= 0;
        if ((was == AnimationPhase_Idle || was == AnimationPhase_Before) && now == AnimationPhase_Active) {
            queueCssEvent(animation, "animationstart", from, start);
        } else if ((was == AnimationPhase_Idle || was == AnimationPhase_Before) && now == AnimationPhase_After) {
            queueCssEvent(animation, "animationstart", from, start);
            queueCssEvent(animation, "animationend", to, end);
        } else if (was == AnimationPhase_Active && now == AnimationPhase_Before) {
            queueCssEvent(animation, "animationend", from, start);
        } else if (was == AnimationPhase_Active && now == AnimationPhase_Active) {
            // An iteration ends: the elapsed time is where the iteration that was current begins.
            double turn = forwards ? iteration - t.iterationStart : animation->eventIteration + 1 - t.iterationStart;
            double elapsed = prF64Max(turn * duration, 0);
            queueCssEvent(animation, "animationiteration", elapsed, timelineAt(animation, t.delay + elapsed));
        } else if (was == AnimationPhase_Active && now == AnimationPhase_After) {
            queueCssEvent(animation, "animationend", to, end);
        } else if (was == AnimationPhase_After && now == AnimationPhase_Active) {
            queueCssEvent(animation, "animationstart", to, end);
        } else if (was == AnimationPhase_After && now == AnimationPhase_Before) {
            queueCssEvent(animation, "animationstart", to, end);
            queueCssEvent(animation, "animationend", from, start);
        }
    }
    animation->eventPhase = now;
    animation->eventIteration = iteration;
}

// ---------------------------------------------------------------------------------------------------------------------
// Keyframes

// The contexts that the target's keyframe values compute in. They match the contexts that the cascade uses for the font
// group. `font-size` is relative to the parent's font, `line-height` to the target's font size and the parent's line
// height, and the other properties to the target's font. The root-relative lengths of the root element advance the same
// way, starting from the initial font.
struct KeyframeContext {
    CssStyleContext fontSize;
    CssStyleContext lineHeight;
    CssStyleContext own;
    const CssComputedStyle* PR_NULLABLE parent;     // the style that inherit takes its values from

    // The target and its presentation, which compute the style that revert rolls back to the first time that a keyframe
    // needs it. The context holds a reference to that style.
    PrCssPresentation* presentation;
    const PrDomNode* target;
    CssAnimateContext animation;
    const CssComputedStyle* PR_NULLABLE reverted;

    const CssStyleContext& of(PrCssProperty property) const {
        return property == PrCssProperty_FontSize ? fontSize : property == PrCssProperty_LineHeight ? lineHeight : own;
    }
};

KeyframeContext keyframeContext(const CssComputedStyle* base, const CssAnimateContext& c) {
    CssStyleContext context{};
    context.font = c.parent != nullptr ? cssStyleFontBases(c.parent) : kCssInitialFontBases;
    context.root = c.parent != nullptr ? c.rootFont : kCssInitialFontBases;
    context.parentWeight = c.parent != nullptr ? cssStyleFontWeight(c.parent) : 400;
    context.parentColor = c.parent != nullptr ? cssStyleColor(c.parent, kCssDefaultColorEnvironment) : PrColor{0, 0, 0, 1};
    context.viewport = c.viewport;
    KeyframeContext contexts{context, context, context, c.parent, nullptr, nullptr, c, nullptr};
    contexts.lineHeight.font = cssFontBases(cssStyleFontSize(base), context.font.lh);
    contexts.own.font = cssStyleFontBases(base);
    if (c.parent == nullptr) {
        contexts.lineHeight.root = cssFontBases(contexts.lineHeight.font.em, context.root.lh);
        contexts.own.root = contexts.own.font;
    }
    return contexts;
}

// Returns the style that revert in a keyframe rolls back to: the animation origin and the author origin together roll
// back to the user agent's (css-cascade-5/#default).
const CssComputedStyle* revertedStyle(KeyframeContext& context) {
    if (context.reverted == nullptr) {
        PrCssPresentation* r = context.presentation;
        CssCascadeInput input{
            .element = originOf(context.target),
            .parent = context.animation.parent,
            .rootFont = context.animation.rootFont,
            .viewport = context.animation.viewport,
            .fresh = true,
            .matched = nullptr,
            .pseudo = pseudoOf(context.target),
            .userAgentOnly = true,
        };
        context.reverted = cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
    }
    return context.reverted;
}

// Computes the effect's keyframes for the target into the effect (css-animations-1/#keyframes,
// web-animations-1/#computing-property-values). A keyframe that declares no composite operation takes the effect's.
void computeKeyframes(PrCssKeyframeEffect* effect, KeyframeContext& context) {
    cssValuePoolReset(&effect->values);
    effect->frames.clear();
    effect->inheritsReset = false;
    effect->computed = true;
    if (effect->keyframes == nullptr)
        return;
    const RawArray<CssKeyframeRecord>& frames = effect->keyframes->frames;
    for (uint32_t c = 0; c < frames.count; ++c) {
        const CssKeyframeRecord& frame = frames.data[c];
        const CssDeclarationBlock* block = frame.style;
        const CssDeclaration* declarations = cssDeclarations(block);
        CssValueSpan easing{0, 0};
        if (frame.easing.count != 0)
            easing = cssValuePoolAddRun(&effect->values, listRun(frame.easing));
        PrCssComposite composite = cssKeyframeComposite(frame.composite, effect->composite);
        // Adds the keyframe's value `run` of the longhand `property`.
        auto addValue = [&](PrCssProperty property, CssValueRun run) {
            CssStyleSlot slot = cssStyleSlot(property);
            if (slot.group == CssStyleGroup_Count || slot.group == CssStyleGroup_Animation)
                return;
            uint32_t start = effect->values.values.count;
            bool base = false;
            if (run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword &&
                    cssIsCssWideKeyword(cssValueKeyword(run.values))) {
                // initial, inherit and unset give the values that they give in a declaration. revert gives the user
                // agent's value, and revert-layer and revert-rule the base value.
                PrCssKeyword keyword = cssValueKeyword(run.values);
                bool inherits = keyword == PrCssKeyword_Inherit ||
                    (keyword == PrCssKeyword_Unset && bitsetTest(kCssPropertyInherited, property));
                if (keyword == PrCssKeyword_RevertLayer || keyword == PrCssKeyword_RevertRule) {
                    base = true;
                } else if (keyword == PrCssKeyword_Revert) {
                    cssValuePoolAddRun(&effect->values, cssComputedValue(revertedStyle(context), property));
                } else if (inherits && context.parent != nullptr) {
                    cssValuePoolAddRun(&effect->values, cssComputedValue(context.parent, property));
                    effect->inheritsReset = effect->inheritsReset || !bitsetTest(kCssPropertyInherited, property);
                } else {
                    cssStyleComputeInitial(&effect->values, property, context.of(property));
                }
            } else {
                cssStyleComputeValue(&effect->values, property, run, context.of(property));
            }
            CssValueSpan value{start, effect->values.values.count - start};
            effect->frames.push(CssAnimateKeyframe{frame.computedOffset, property, value, easing, composite, base});
        };
        for (uint32_t d = 0; d < block->count; ++d) {
            const CssDeclaration* declaration = &declarations[d];
            PrCssProperty property = declaration->property;
            // Important declarations in a keyframe are ignored (css-animations-1/#keyframes).
            if (property >= PrCssProperty_Count || declaration->important)
                continue;
            CssValueRun run = cssDeclarationValue(block, declaration);
            if (run.count == 0 || run.values[0].kind == PrCssValueKind_Tokens)
                continue;
            // A shorthand's values come as its longhands, except a CSS-wide keyword, which the shorthand keeps and
            // which gives each of its longhands that keyword.
            CssPropertySpan longhands = kCssPropertyLonghands[property];
            if (longhands.count == 0) {
                addValue(property, run);
            } else if (run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword &&
                    cssIsCssWideKeyword(cssValueKeyword(run.values))) {
                for (uint32_t l = 0; l < longhands.count; ++l)
                    addValue(kCssPropertyRuns[longhands.start + l], run);
            }
        }
    }
    // Insertion keeps keyframes of equal offsets in their order.
    for (uint32_t i = 1; i < effect->frames.count; ++i) {
        CssAnimateKeyframe moving = effect->frames.data[i];
        uint32_t k = i;
        for (; k != 0 && effect->frames.data[k - 1].offset > moving.offset; --k)
            effect->frames.data[k] = effect->frames.data[k - 1];
        effect->frames.data[k] = moving;
    }
}

// Appends the effect's value of `property` at `progress` over `underlying`, which stands in at the offsets that no
// keyframe gives (web-animations-1/#the-effect-value-of-a-keyframe-animation-effect). A keyframe that adds or
// accumulates is composited onto `underlying` before the keyframes interpolate. Returns false if no keyframe gives the
// property.
bool addEffectValue(CssValuePool* out, const PrCssKeyframeEffect* effect, PrCssProperty property, double progress,
        CssValueRun underlying, CssValueRun base, const CssInterpolationContext& interpolation) {
    struct Frame {
        double offset;
        CssValueRun value;
        CssValueRun easing;
        CssValueSpan composited;
        bool pooled;
    };
    Frame frames[32];
    uint32_t count = 0;
    bool any = false;
    CssValueRun none{nullptr, 0, nullptr};
    CssValuePool composited{};
    for (uint32_t i = 0; i < effect->frames.count && count + 1 < 32; ++i) {
        const CssAnimateKeyframe& k = effect->frames.data[i];
        if (k.property != property)
            continue;
        if (!any && k.offset > 0)
            frames[count++] = Frame{0, underlying, none, {}, false};
        any = true;
        Frame frame{
            k.offset, k.base ? base : cssValuePoolRun(&effect->values, k.value),
            cssValuePoolRun(&effect->values, k.easing), {}, false,
        };
        if (k.composite != PrCssComposite_Replace) {
            uint32_t start = composited.values.count;
            cssComposite(&composited, property, underlying, frame.value, k.composite, interpolation);
            frame.composited = CssValueSpan{start, composited.values.count - start};
            frame.pooled = true;
        }
        frames[count++] = frame;
    }
    if (!any)
        return false;
    if (frames[count - 1].offset < 1)
        frames[count++] = Frame{1, underlying, none, {}, false};
    for (uint32_t i = 0; i < count; ++i) {
        if (frames[i].pooled)
            frames[i].value = cssValuePoolRun(&composited, frames[i].composited);
    }
    // web-animations-1/#the-effect-value-of-a-keyframe-animation-effect: a progress outside [0, 1] interpolates from
    // the first or the last pair of keyframes at those offsets, and inside from the last keyframe at or before it.
    uint32_t at = 0;
    if (progress >= 1) {
        at = count >= 2 ? count - 2 : 0;
        while (at > 0 && frames[at].offset == 1)
            --at;
        // Several keyframes at offset 1 leave the last of them as the value. A single one is interpolated to, which
        // keeps the form that interpolation gives, such as a repeatable list repeated to both lists' lengths.
        if (count >= 2 && frames[count - 2].offset == 1 && frames[count - 1].offset == 1 && progress == 1) {
            uint32_t last = count - 1;
            cssValuePoolAddRun(out, frames[last].value);
            cssValuePoolReset(&composited);
            return true;
        }
    } else if (progress >= 0) {
        while (at + 1 < count && frames[at + 1].offset <= progress)
            ++at;
        if (at + 1 == count)
            at = count >= 2 ? count - 2 : 0;
    }
    const Frame& from = frames[at];
    const Frame& to = at + 1 < count ? frames[at + 1] : frames[at];
    double span = to.offset - from.offset;
    double local = span > 0 ? (progress - from.offset) / span : 1;
    CssValueRun easing = from.easing.count != 0 ? from.easing : listRun(effect->defaultEasing);
    PrLayoutEasing eased = easing.count != 0 ? cssEasing(easing) : prLayoutEasingLinear();
    cssInterpolate(out, property, from.value, to.value, prLayoutEase(&eased, local, false), interpolation);
    cssValuePoolReset(&composited);
    return true;
}

// Records that the property's value is the span of the scratch pool, replacing an earlier override.
void override(CssAnimations& a, PrCssProperty property, CssValueSpan span) {
    for (uint32_t i = 0; i < a.overridden.count; ++i) {
        if (a.overridden.data[i] == property) {
            a.overrides.data[i] = span;
            return;
        }
    }
    a.overridden.push(property);
    a.overrides.push(span);
}

// Returns the value of `property` that the effects below have given so far, or `base`.
CssValueRun underlyingOf(const CssAnimations& a, PrCssProperty property, CssValueRun base) {
    for (uint32_t i = 0; i < a.overridden.count; ++i) {
        if (a.overridden.data[i] == property)
            return cssValuePoolRun(&a.scratch, a.overrides.data[i]);
    }
    return base;
}

// Returns true if a compositor can run an animation of the target's opacity or transform, which it shows on the
// target's box as a layer. An element of SVG content has no box: the content of an svg element draws its children. The
// parent is the flat tree's, so an instance in a use element's shadow tree is SVG content of the use.
bool composites(const PrDomElement* element) {
    const PrDomElement* parent = domElementFromNode(domFlatParent(element));
    return element->space != PrDomNamespace_Svg || parent == nullptr || parent->space != PrDomNamespace_Svg;
}

// Returns true if a compositor can run the animation's effect: its keyframes set only opacity and transform, replace
// what is under them, and no important declaration of the target wins over them.
bool compositedEffect(const PrCssAnimation* animation, const CssComputedStyle* base) {
    const PrCssKeyframeEffect* effect = animation->effect;
    if (effect == nullptr || effect->target == nullptr || effect->frames.count == 0 || !composites(effect->target) ||
            effect->iterationComposite != PrCssComposite_Replace)
        return false;
    for (uint32_t k = 0; k < effect->frames.count; ++k) {
        const CssAnimateKeyframe& frame = effect->frames.data[k];
        bool effectProperty = frame.property == PrCssProperty_Opacity || frame.property == PrCssProperty_Transform;
        if (!effectProperty || frame.composite != PrCssComposite_Replace || frame.base ||
                cssStyleIsImportant(base, frame.property))
            return false;
    }
    return true;
}

// Returns true if the effect's target keeps its box a layer for a composited effect.
bool compositedNow(const PrCssPresentation* presentation, const PrCssKeyframeEffect* effect) {
    const PrDomNode* target = effect->target;
    if (effect->pseudo != CssPseudo_None)
        target = cssPresentationPseudoNode(presentation, effect->target, effect->pseudo);
    return target != nullptr && cssAnimateComposited(presentation, target);
}

// Makes the effect of a CSS animation or transition of `target`, with its timing from the style, and an animation that
// plays it on the document's timeline, which the presentation owns.
PrCssAnimation* makeOwned(PrCssPresentation* presentation, const PrDomNode* target, PrCssAnimationKind kind) {
    PrDomDocument* document = presentation->document;
    PrCssAnimation* a = cssAnimationMake(document, kind);
    a->owner = presentation;
    PrCssKeyframeEffect* effect = allocZeroed<PrCssKeyframeEffect>();
    effect->references = 1;
    domNodeRetain(document);
    effect->document = document;
    PrDomElement* element = const_cast<PrDomElement*>(originOf(target));
    domNodeRetain(element);
    effect->target = element;
    effect->pseudo = pseudoOf(target);
    effect->pseudoText = prRawStringNull();
    if (effect->pseudo != CssPseudo_None)
        effect->pseudoText = prRawStringCopy(cssPseudoName(effect->pseudo));
    effect->timing = prLayoutEffectTimingDefault();
    effect->duration = 0;
    effect->fill = PrCssFillMode_None;
    effect->easing = prRawStringCopy("linear");
    a->effect = effect;
    effect->animation = a;
    cssAnimationIndexEffect(effect);
    // The target is being styled, which applies the effect without a restyle of its own.
    PrCssTimeline* timeline = prCssDocumentTimeline(document);
    prCssTimelineRetain(timeline);
    a->timeline = timeline;
    return a;
}

// Sets the easing of the effect's timing from a computed easing function, with its serialization.
void setEffectEasing(PrCssKeyframeEffect* effect, CssValueRun easing) {
    effect->timing.easing = easing.count != 0 ? cssEasing(easing) : prLayoutEasingLinear();
    PrTextBuffer text{};
    if (easing.count != 0)
        cssSerializeValues(&text, easing);
    else
        prTextBufferAppend(&text, "linear");
    prRawStringReplace(&effect->easing, prTextBufferView(&text));
    prTextBufferReset(&text);
}

// Ends the presentation's ownership of the animation, which a script may still hold, and cancels it.
void disown(PrCssAnimation* animation) {
    cssAnimationCancel(animation);
    animation->owner = nullptr;
    cssAnimationRelist(animation);
}

// The transition of `property` that the presentation runs on `target`, or null.
PrCssAnimation* PR_NULLABLE transitionOf(PrCssPresentation* presentation, const PrDomNode* target,
        PrCssProperty property) {
    return ownedOf(presentation, target, [&](PrCssAnimation* a) {
        return a->kind == PrCssAnimationKind_Transition && a->property == property;
    });
}

// Returns true if the transition still plays its endpoints, which a script may have replaced with another effect.
bool hasEndpoints(const PrCssAnimation* animation) {
    const PrCssKeyframeEffect* effect = animation->effect;
    return effect != nullptr && effect->keyframes == nullptr && effect->frames.count == 2;
}

// Appends the transition's value now, over nothing: a transition replaces the value below it.
void addTransitionValue(CssValuePool* out, const PrCssAnimation* animation,
        const CssInterpolationContext& interpolation) {
    const PrCssKeyframeEffect* effect = animation->effect;
    PrLayoutComputedTiming timing = cssAnimationComputedTiming(animation);
    double progress = prF64IsNan(timing.progress) ? 0 : timing.progress;
    CssValueRun from = cssValuePoolRun(&effect->values, effect->frames.data[0].value);
    CssValueRun to = cssValuePoolRun(&effect->values, effect->frames.data[1].value);
    cssInterpolate(out, animation->property, from, to, progress, interpolation);
}

// Returns the last item of the target's transition-property list that names `property`, or -1 if none does
// (css-transitions-1/#transition-property-property).
int32_t transitionItem(CssValueRun list, const CssListItems& items, PrCssProperty property) {
    int32_t found = -1;
    for (uint32_t i = 0; i < items.count; ++i) {
        CssValueRun item = cssListItem(list, items, i);
        if (item.count != 1)
            continue;
        const CssValueNode* value = item.values;
        if (cssValueIsKeyword(value, PrCssKeyword_All)) {
            found = int32_t(i);
            continue;
        }
        PrStringView name;
        if (value->kind == PrCssValueKind_Keyword)
            name = cssKeywordName(cssValueKeyword(value));
        else if (value->kind == PrCssValueKind_Ident)
            name = cssValueText(item, value);
        else
            name = PrStringView{};
        PrCssProperty named = cssPropertyFromText(name);
        if (named == property)
            found = int32_t(i);
        // A shorthand names its longhands.
        CssPropertySpan longhands = named < PrCssProperty_Count ? kCssPropertyLonghands[named] : CssPropertySpan{};
        for (uint32_t k = 0; k < longhands.count; ++k) {
            if (kCssPropertyRuns[longhands.start + k] == property)
                found = int32_t(i);
        }
    }
    return found;
}

// Returns the name that an item of animation-name gives, or the empty view for `none`.
PrStringView animationName(CssValueRun item) {
    if (item.count != 1)
        return "";
    const CssValueNode* value = item.values;
    if (value->kind == PrCssValueKind_Ident || value->kind == PrCssValueKind_String)
        return cssValueText(item, value);
    if (value->kind == PrCssValueKind_Keyword && cssValueKeyword(value) != PrCssKeyword_None)
        return cssKeywordName(cssValueKeyword(value));
    return "";
}

// Starts, reverses and cancels the target's transitions (css-transitions-1/#starting).
void restyleTransitions(PrCssPresentation* presentation, const PrDomNode* target, const CssComputedStyle* PR_NULLABLE oldBase,
        const CssComputedStyle* PR_NULLABLE old, const CssComputedStyle* base,
        const CssInterpolationContext& interpolation) {
    CssValueRun properties = cssComputedValue(base, PrCssProperty_TransitionProperty);
    CssListItems propertyItems = cssListItems(properties);
    CssValueRun durations = cssComputedValue(base, PrCssProperty_TransitionDuration);
    CssListItems durationItems = cssListItems(durations);
    CssValueRun delays = cssComputedValue(base, PrCssProperty_TransitionDelay);
    CssListItems delayItems = cssListItems(delays);
    CssValueRun easings = cssComputedValue(base, PrCssProperty_TransitionTimingFunction);
    CssListItems easingItems = cssListItems(easings);
    CssValueRun behaviors = cssComputedValue(base, PrCssProperty_TransitionBehavior);
    CssListItems behaviorItems = cssListItems(behaviors);
    bool hidden = displaysNone(base) || (oldBase != nullptr && displaysNone(oldBase));
    CssValuePool value{};
    for (CssStyleGroup group : kAnimatedGroups) {
        CssStyleGroupProperties members = cssStyleGroupProperties(group);
        for (uint32_t p = 0; p < members.count; ++p) {
            PrCssProperty property = members.properties[p];
            PrCssAnimation* running = transitionOf(presentation, target, property);
            int32_t item = transitionItem(properties, propertyItems, property);
            if (item < 0 || hidden || oldBase == nullptr) {
                if (running != nullptr)
                    disown(running);
                continue;
            }
            CssValueRun end = cssComputedValue(base, property);
            if (running != nullptr && cssValueRunEquals(listRun(running->transitionTo), end))
                continue;
            // The before-change value is what the running transition shows now, or else the old style's value.
            value.values.count = 0;
            value.text.count = 0;
            CssValueRun start{};
            if (running != nullptr && hasEndpoints(running)) {
                addTransitionValue(&value, running, interpolation);
                start = cssValuePoolRun(&value, CssValueSpan{0, value.values.count});
            } else {
                start = cssComputedValue(old != nullptr ? old : oldBase, property);
            }
            double duration = prF64Max(cssTimeMilliseconds(cssListItem(durations, durationItems, uint32_t(item)), 0), 0);
            double delay = cssTimeMilliseconds(cssListItem(delays, delayItems, uint32_t(item)), 0);
            // A value that interpolates only discretely transitions only if the behavior allows it
            // (css-transitions-2/#transition-behavior-property).
            CssValueRun behavior = cssListItem(behaviors, behaviorItems, uint32_t(item));
            bool discrete = behavior.count == 1 && cssValueIsKeyword(behavior.values, PrCssKeyword_AllowDiscrete);
            bool starts = duration + delay > 0 && (cssInterpolable(property, start, end, interpolation) ||
                (discrete && !cssValueRunEquals(start, end)));
            if (!starts) {
                if (running != nullptr)
                    disown(running);
                continue;
            }
            // A transition back to where the running one started is shortened by how far that one got
            // (css-transitions-1/#reversing).
            double factor = 1;
            CssValueList reversingStart{};
            if (running != nullptr && hasEndpoints(running) && cssValueRunEquals(listRun(running->reversingStart), end)) {
                PrLayoutComputedTiming timing = cssAnimationComputedTiming(running);
                double progress = prF64IsNan(timing.progress) ? 0 : timing.progress;
                factor = prF64Min(prF64Max(prF64Abs(progress * running->reversingFactor + 1 - running->reversingFactor), 0), 1);
                reversingStart = cssValueListCopy(listRun(running->transitionTo));
                if (delay < 0)
                    delay *= factor;
                duration *= factor;
            } else {
                reversingStart = cssValueListCopy(start);
            }
            PrCssAnimation* made = makeOwned(presentation, target, PrCssAnimationKind_Transition);
            made->property = property;
            made->transitionTo = cssValueListCopy(end);
            made->reversingStart = reversingStart;
            made->reversingFactor = factor;
            PrCssKeyframeEffect* effect = made->effect;
            effect->timing.duration = duration;
            effect->timing.delay = delay;
            effect->duration = duration;
            // A transition fills backwards through its delay (css-transitions-1/#application).
            effect->timing.fill = PrLayoutFill_Backwards;
            effect->fill = PrCssFillMode_Backwards;
            setEffectEasing(effect, cssListItem(easings, easingItems, uint32_t(item)));
            // The endpoints are the transition's keyframes.
            CssValueSpan from = cssValuePoolAddRun(&effect->values, start);
            CssValueSpan to = cssValuePoolAddRun(&effect->values, end);
            effect->frames.push(CssAnimateKeyframe{0, property, from, {}, PrCssComposite_Replace, false});
            effect->frames.push(CssAnimateKeyframe{1, property, to, {}, PrCssComposite_Replace, false});
            effect->computed = true;
            made->composited = (property == PrCssProperty_Opacity || property == PrCssProperty_Transform) &&
                composites(effect->target);
            // A new transition of the property cancels the one running (css-transitions-1/#starting).
            if (running != nullptr)
                disown(running);
            cssAnimationPlay(made, false);
            prCssAnimationRelease(made);
        }
    }
    cssValuePoolReset(&value);
}

PrLayoutPlaybackDirection directionOf(CssValueRun item) {
    switch (cssValueRunKeyword(item)) {
        case PrCssKeyword_Reverse: return PrLayoutPlaybackDirection_Reverse;
        case PrCssKeyword_Alternate: return PrLayoutPlaybackDirection_Alternate;
        case PrCssKeyword_AlternateReverse: return PrLayoutPlaybackDirection_AlternateReverse;
        default: return PrLayoutPlaybackDirection_Normal;
    }
}

PrCssFillMode fillOf(CssValueRun item) {
    switch (cssValueRunKeyword(item)) {
        case PrCssKeyword_Forwards: return PrCssFillMode_Forwards;
        case PrCssKeyword_Backwards: return PrCssFillMode_Backwards;
        case PrCssKeyword_Both: return PrCssFillMode_Both;
        default: return PrCssFillMode_None;
    }
}

// Starts, updates and cancels the target's CSS animations (css-animations-1/#animations, css-animations-2/#animations).
// Each item of animation-name that names keyframes is matched by name to the target's running animations.
void restyleAnimations(PrCssPresentation* presentation, const PrDomNode* target, const CssComputedStyle* base, bool hidden) {
    CssValueRun names = cssComputedValue(base, PrCssProperty_AnimationName);
    CssListItems nameItems = cssListItems(names);
    if (const RawArray<PrCssKeyframeEffect*>* effects = effectsOf(presentation, target)) {
        for (uint32_t i = 0; i < effects->count; ++i)
            effects->data[i]->animation->matched = false;
    }
    for (uint32_t n = 0; n < nameItems.count && !hidden; ++n) {
        PrStringView name = animationName(cssListItem(names, nameItems, n));
        CssKeyframes* keyframes = name.size != 0 ? cssRuleSetKeyframes(&presentation->rules, name) : nullptr;
        if (keyframes == nullptr)
            continue;
        PrCssAnimation* a = ownedOf(presentation, target, [&](PrCssAnimation* candidate) {
            return candidate->kind == PrCssAnimationKind_Css && !candidate->matched && prRawStringView(&candidate->name) == name;
        });
        bool made = a == nullptr;
        if (made) {
            a = makeOwned(presentation, target, PrCssAnimationKind_Css);
            a->name = prRawStringCopy(name);
        }
        a->matched = true;
        a->nameIndex = n;
        auto item = [&](PrCssProperty property) {
            CssValueRun list = cssComputedValue(base, property);
            return cssListItem(list, cssListItems(list), n);
        };
        PrCssKeyframeEffect* effect = a->effect;
        PrLayoutEffectTiming& t = effect->timing;
        PrCssTimingMembers overridden = a->timingOverridden;
        if ((overridden & PrCssTimingMember_Duration) == 0) {
            t.duration = prF64Max(cssTimeMilliseconds(item(PrCssProperty_AnimationDuration), 0), 0);
            effect->duration = t.duration;
        }
        if ((overridden & PrCssTimingMember_Delay) == 0)
            t.delay = cssTimeMilliseconds(item(PrCssProperty_AnimationDelay), 0);
        if ((overridden & PrCssTimingMember_Iterations) == 0) {
            CssValueRun count = item(PrCssProperty_AnimationIterationCount);
            if (cssValueRunKeyword(count) == PrCssKeyword_Infinite)
                t.iterations = kPrF64Infinity;
            else if (count.count == 1 && cssValueIsNumeric(count.values[0].kind) && count.values[0].number >= 0)
                t.iterations = count.values[0].number;
            else
                t.iterations = 1;
        }
        if ((overridden & PrCssTimingMember_Direction) == 0)
            t.direction = directionOf(item(PrCssProperty_AnimationDirection));
        if ((overridden & PrCssTimingMember_Fill) == 0) {
            effect->fill = fillOf(item(PrCssProperty_AnimationFillMode));
            t.fill = PrLayoutFill(effect->fill);
        }
        // The animation-timing-function eases each keyframe that gives no easing, and the effect is linear
        // (css-animations-2/#keyframes).
        if (!a->keyframesOverridden) {
            if (effect->keyframes != keyframes) {
                cssKeyframesRetain(keyframes);
                cssKeyframesRelease(effect->keyframes);
                effect->keyframes = keyframes;
            }
            cssValueListReset(&effect->defaultEasing);
            effect->defaultEasing = cssValueListCopy(item(PrCssProperty_AnimationTimingFunction));
            effect->composite = cssCompositeFromValue(item(PrCssProperty_AnimationComposition));
        }
        effect->computed = false;
        bool paused = cssValueRunKeyword(item(PrCssProperty_AnimationPlayState)) == PrCssKeyword_Paused;
        if (made) {
            cssAnimationPlay(a, false);
            if (paused)
                cssAnimationPause(a);
            a->cssPaused = paused;
            prCssAnimationRelease(a);
        } else if (!a->playStateOverridden && paused != a->cssPaused) {
            if (paused)
                cssAnimationPause(a);
            else
                cssAnimationPlay(a, false);
            a->cssPaused = paused;
        } else {
            cssAnimationUpdateFinishedState(a, false, false);
        }
    }
    // The animations that no item names are canceled, and their style no longer owns them.
    if (const RawArray<PrCssKeyframeEffect*>* effects = effectsOf(presentation, target)) {
        RawArray<PrCssAnimation*> unmatched;
        for (uint32_t i = 0; i < effects->count; ++i) {
            PrCssAnimation* a = effects->data[i]->animation;
            if (a->owner == presentation && a->kind == PrCssAnimationKind_Css && !a->matched)
                unmatched.push(a);
        }
        for (uint32_t i = 0; i < unmatched.count; ++i)
            disown(unmatched.data[i]);
        unmatched.reset();
    }
}

} // namespace

void cssAnimateRestyled(PrCssPresentation* presentation, const PrDomNode* target,
        const CssComputedStyle* PR_NULLABLE oldBase, const CssComputedStyle* PR_NULLABLE old, const CssComputedStyle* base,
        const CssAnimateContext& context, const CssColorEnvironment& environment) {
    CssInterpolationContext interpolation = cssStyleInterpolationContext(base, environment);
    restyleTransitions(presentation, target, oldBase, old, base, interpolation);
    restyleAnimations(presentation, target, base, displaysNone(base));
    // Every effect that targets the target computes its keyframes for its new style.
    RawArray<PrCssKeyframeEffect*> stack;
    stackOf(presentation, target, stack);
    if (stack.count != 0) {
        KeyframeContext keyframes = keyframeContext(base, context);
        keyframes.presentation = presentation;
        keyframes.target = target;
        for (uint32_t i = 0; i < stack.count; ++i) {
            PrCssKeyframeEffect* effect = stack.data[i];
            PrCssAnimation* a = effect->animation;
            if (cssEffectHasEndpoints(effect))
                continue;
            computeKeyframes(effect, keyframes);
            a->composited = a->owner == presentation && compositedEffect(a, base);
        }
        cssComputedStyleRelease(keyframes.reverted);
    }
    stack.reset();
}

const CssComputedStyle* cssAnimateApply(PrCssPresentation* presentation, const PrDomNode* target,
        const CssComputedStyle* base, bool composited, const CssColorEnvironment& environment) {
    CssAnimations& a = animationsOf(presentation);
    a.scratch.values.count = 0;
    a.scratch.text.count = 0;
    a.overridden.clear();
    a.overrides.clear();
    CssInterpolationContext interpolation = cssStyleInterpolationContext(base, environment);
    bool inheritsReset = false;
    RawArray<PrCssKeyframeEffect*> stack;
    stackOf(presentation, target, stack);
    // The effects apply in composite order, each over what those below it give: the animation origin first, which
    // important declarations override, and then the transitions over it (css-cascade-5/#cascade-origin).
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < stack.count; ++i) {
            PrCssKeyframeEffect* effect = stack.data[i];
            const PrCssAnimation* animation = effect->animation;
            bool transition = animation->kind == PrCssAnimationKind_Transition && animation->owner != nullptr;
            if (transition != (pass == 1) || !effect->computed || (animation->composited && !composited) ||
                    animation->replaceState == PrCssReplaceState_Removed)
                continue;
            PrLayoutComputedTiming timing = cssAnimationComputedTiming(animation);
            if (prF64IsNan(timing.progress))
                continue;
            inheritsReset = inheritsReset || effect->inheritsReset;
            for (uint32_t k = 0; k < effect->frames.count; ++k) {
                PrCssProperty property = effect->frames.data[k].property;
                if (!transition && cssStyleIsImportant(base, property))
                    continue;
                bool seen = false;
                for (uint32_t j = 0; j < k && !seen; ++j)
                    seen = effect->frames.data[j].property == property;
                if (seen)
                    continue;
                CssValueRun under = cssComputedValue(base, property);
                CssValueRun underlying = transition ? under : underlyingOf(a, property, under);
                // The underlying run may refer to the scratch pool, which grows as the value is added. It is copied
                // first.
                CssValuePool held{};
                if (underlying.values != under.values) {
                    cssValuePoolAddRun(&held, underlying);
                    underlying = cssValuePoolRun(&held, CssValueSpan{0, held.values.count});
                }
                uint32_t start = a.scratch.values.count;
                if (addEffectValue(&a.scratch, effect, property, timing.progress, underlying, under, interpolation))
                    override(a, property, CssValueSpan{start, a.scratch.values.count - start});
                cssValuePoolReset(&held);
            }
        }
    }
    stack.reset();
    if (a.overridden.count == 0) {
        cssComputedStyleRetain(base);
        return base;
    }
    CssStyleBuilder* builder = &presentation->threads.data[0].builder;
    CssComputedStyle* style = allocZeroed<CssComputedStyle>();
    style->references.storeRelaxed(1);
    style->custom = base->custom;
    if (style->custom != nullptr)
        cssCustomPropertiesRetain(style->custom);
    style->important = base->important;
    if (style->important != nullptr)
        cssImportantPropertiesRetain(style->important);
    style->locale = base->locale;
    style->appearance = base->appearance;
    style->inheritsReset = base->inheritsReset || inheritsReset;
    style->staticInline = base->staticInline;
    style->childrenAreItems = base->childrenAreItems;
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        CssStyleGroupProperties members = cssStyleGroupProperties(CssStyleGroup(g));
        bool any = false;
        for (uint32_t o = 0; o < a.overridden.count && !any; ++o)
            any = cssStyleSlot(a.overridden.data[o]).group == g;
        if (!any) {
            cssStyleValuesRetain(base->groups[g]);
            style->groups[g] = base->groups[g];
            continue;
        }
        cssStyleBegin(builder);
        for (uint32_t p = 0; p < members.count; ++p) {
            CssValueRun run = cssStyleValue(base->groups[g], p);
            for (uint32_t o = 0; o < a.overridden.count; ++o) {
                if (a.overridden.data[o] == members.properties[p])
                    run = cssValuePoolRun(&a.scratch, a.overrides.data[o]);
            }
            cssStyleAddValue(builder, run);
        }
        style->groups[g] = cssStyleEnd(builder, CssStyleGroup(g));
    }
    cssComputedStyleSetFontBases(style);
    return style;
}

bool cssAnimateComposited(const PrCssPresentation* presentation, const PrDomNode* target) {
    const RawArray<PrCssKeyframeEffect*>* effects = effectsOf(presentation, target);
    for (uint32_t i = 0; effects != nullptr && i < effects->count; ++i) {
        const PrCssAnimation* a = effects->data[i]->animation;
        if (!a->composited)
            continue;
        PrLayoutComputedTiming timing = cssAnimationComputedTiming(a);
        // An effect that has not reached its end, or that fills past it, keeps the target a layer.
        if (!prF64IsNan(timing.progress) || timing.phase == PrLayoutEffectPhase_Before ||
                (timing.phase != PrLayoutEffectPhase_None && prCssAnimationPlayState(a) != PrCssPlayState_Finished))
            return true;
    }
    return false;
}

const RawArray<PrLayoutTrack>& cssAnimateTracks(PrCssPresentation* presentation, AnimationNodeFn layoutNode,
        AnimationBaseFn baseOf) {
    CssAnimations& a = animationsOf(presentation);
    a.tracks.clear();
    a.keyframes.clear();
    // A keyframe of a composited effect, from a computed value. A transform's keyframe holds the base's individual
    // transforms before the animated list, which interpolate as equal functions.
    auto frame = [](double offset, PrCssProperty property, CssValueRun value, PrLayoutEasing easing,
            const CssComputedStyle* base) {
        PrLayoutKeyframe made{};
        made.offset = offset;
        made.easing = easing;
        made.opacity = value.count == 1 && cssValueIsNumeric(value.values[0].kind) ? float(value.values[0].number) : 1;
        if (property == PrCssProperty_Transform)
            made.transform = cssTransformFunctions(cssComputedValue(base, PrCssProperty_Translate),
                cssComputedValue(base, PrCssProperty_Rotate), cssComputedValue(base, PrCssProperty_Scale), value);
        return made;
    };
    // Tracks refer to their keyframes by index until every keyframe is made, because the keyframes move while they are
    // added.
    RawArray<uint32_t> starts;
    RawArray<PrCssKeyframeEffect*> stack;
    for (uint32_t t = 0; t < a.targets.count; ++t) {
        const CssAnimatedTarget& animated = a.targets.data[t];
        const RawArray<PrCssKeyframeEffect*>& effects = animated.effects;
        stack.clear();
        stack.append(effects.data, effects.count);
        sortItems(stack.span(), [](PrCssKeyframeEffect* x, PrCssKeyframeEffect* y) {
            return cssAnimationBefore(x->animation, y->animation);
        });
        for (uint32_t i = 0; i < stack.count; ++i) {
            const PrCssKeyframeEffect* effect = stack.data[i];
            const PrCssAnimation* animation = effect->animation;
            if (!animation->composited || !effect->computed || animation->replaceState == PrCssReplaceState_Removed ||
                    (animation->pending == CssPendingTask_None && prF64IsNan(cssAnimationCurrentTime(animation))))
                continue;
            const PrDomNode* target = animated.element;
            if (animated.pseudo != CssPseudo_None) {
                target = cssPresentationPseudoNode(presentation, animated.element, animated.pseudo);
                if (target == nullptr)
                    continue;
            }
            PrLayoutNode node = layoutNode(presentation, target);
            const CssComputedStyle* base = node != kPrLayoutNodeNone ? baseOf(presentation, target) : nullptr;
            if (base == nullptr)
                continue;
            CssValueRun defaultEasing = listRun(effect->defaultEasing);
            PrLayoutEasing easing = defaultEasing.count != 0 ? cssEasing(defaultEasing) : prLayoutEasingLinear();
            const PrCssProperty effects2[] = {PrCssProperty_Opacity, PrCssProperty_Transform};
            for (PrCssProperty property : effects2) {
                uint32_t start = a.keyframes.count;
                CssValueRun underlying = cssComputedValue(base, property);
                for (uint32_t k = 0; k < effect->frames.count; ++k) {
                    const CssAnimateKeyframe& key = effect->frames.data[k];
                    if (key.property != property)
                        continue;
                    // The base value stands in at the offsets that no keyframe gives.
                    if (a.keyframes.count == start && key.offset > 0)
                        a.keyframes.push(frame(0, property, underlying, easing, base));
                    CssValueRun own = cssValuePoolRun(&effect->values, key.easing);
                    a.keyframes.push(frame(key.offset, property, cssValuePoolRun(&effect->values, key.value),
                        own.count != 0 ? cssEasing(own) : easing, base));
                }
                if (a.keyframes.count == start)
                    continue;
                if (a.keyframes.data[a.keyframes.count - 1].offset < 1)
                    a.keyframes.push(frame(1, property, underlying, easing, base));
                PrLayoutTrack track{};
                track.node = node;
                track.effect = property == PrCssProperty_Opacity ? PrLayoutEffect_Opacity : PrLayoutEffect_Transform;
                track.playback = animation->playback;
                // A pending play starts at the time that its task will give it, once a frame has rendered it, and a
                // pending pause holds.
                if (animation->pending == CssPendingTask_Play && animation->hasPendingRate)
                    track.playback.rate = animation->pendingRate;
                if (animation->pending == CssPendingTask_Play && !prF64IsNan(track.playback.holdTime) &&
                        a.renders > animation->pendingRender && track.playback.rate != 0) {
                    track.playback.startTime = cssAnimationReadyTime(animation, a.time) -
                        track.playback.holdTime / track.playback.rate;
                    track.playback.holdTime = kPrF64Nan;
                } else if (animation->pending != CssPendingTask_None) {
                    track.playback.holdTime = cssAnimationCurrentTime(animation);
                }
                if (animation->timeline != nullptr && !prF64IsNan(track.playback.startTime))
                    track.playback.startTime += animation->timeline->originTime;
                track.timing = effect->timing;
                track.keyframeCount = a.keyframes.count - start;
                a.tracks.push(track);
                starts.push(start);
            }
        }
    }
    stack.reset();
    for (uint32_t i = 0; i < a.tracks.count; ++i)
        a.tracks.data[i].keyframes = a.keyframes.data + starts.data[i];
    starts.reset();
    return a.tracks;
}

bool cssAnimateHas(const PrCssPresentation* presentation, const PrDomNode* target) {
    return effectsOf(presentation, target) != nullptr;
}

void cssAnimateForget(PrCssPresentation* presentation, const PrDomNode* target) {
    const RawArray<PrCssKeyframeEffect*>* effects = effectsOf(presentation, target);
    if (effects == nullptr)
        return;
    RawArray<PrCssAnimation*> owned;
    for (uint32_t i = 0; i < effects->count; ++i) {
        PrCssKeyframeEffect* effect = effects->data[i];
        effect->computed = cssEffectHasEndpoints(effect);
        if (effect->animation->owner == presentation)
            owned.push(effect->animation);
    }
    for (uint32_t i = 0; i < owned.count; ++i)
        disown(owned.data[i]);
    owned.reset();
}

void cssAnimateAttach(PrCssPresentation* presentation) {
    animationsOf(presentation).active = true;
}

void cssAnimateDetach(PrCssPresentation* presentation) {
    CssAnimations& animations = animationsOf(presentation);
    RawArray<PrCssAnimation*> owned;
    for (uint32_t i = 0; i < animations.list.count; ++i) {
        if (animations.list.data[i]->owner == presentation)
            owned.push(animations.list.data[i]);
    }
    for (uint32_t i = 0; i < owned.count; ++i) {
        owned.data[i]->owner = nullptr;
        cssAnimationCancel(owned.data[i]);
    }
    owned.reset();
    animations.active = false;
    RawArray<PrCssAnimation*> listed = animations.list;
    animations.list = {};
    for (uint32_t i = 0; i < listed.count; ++i) {
        listed.data[i]->listed = false;
        if (animations.callback.listed != nullptr)
            animations.callback.listed(animations.callback.context, listed.data[i], false);
        prCssAnimationRelease(listed.data[i]);
    }
    listed.reset();
    for (uint32_t i = 0; i < animations.events.count; ++i)
        cssAnimateEventReset(&animations.events.data[i]);
    animations.events.clear();
}

void cssAnimateUpdate(PrCssPresentation* presentation, double time) {
    CssAnimations& a = animationsOf(presentation);
    a.active = true;
    bool moved = time != a.time;
    a.time = time;
    // The list can grow as the steps run, and animations leave it only at the end.
    RawArray<PrCssAnimation*> list;
    list.append(a.list.data, a.list.count);
    for (uint32_t i = 0; i < list.count; ++i)
        prCssAnimationRetain(list.data[i]);
    for (uint32_t i = 0; i < list.count; ++i) {
        PrCssAnimation* animation = list.data[i];
        bool changed = false;
        // web-animations-1/#update-animations-and-send-events: the pending tasks of the animations that are ready run
        // at the timeline's time, and then each updates its finished state.
        if (animation->pending != CssPendingTask_None && animation->timeline != nullptr &&
                !prF64IsNan(prCssTimelineCurrentTime(animation->timeline))) {
            cssAnimationRunPendingTask(animation, cssAnimationReadyTime(animation, time));
            changed = true;
        }
        // An effect moves if its playback moved before the finished state held it.
        bool moves = moved && prLayoutPlaybackMoves(&animation->playback);
        PrCssPlayState before = prCssAnimationPlayState(animation);
        cssAnimationUpdateFinishedState(animation, false, false);
        changed = changed || before != prCssAnimationPlayState(animation);
        // A transition that completed is no longer the running transition of its property, and its style no longer runs
        // it (css-transitions-1/#completed-transition).
        if (animation->owner == presentation && animation->kind == PrCssAnimationKind_Transition &&
                prCssAnimationPlayState(animation) == PrCssPlayState_Finished) {
            findCssEvents(animation);
            animation->owner = nullptr;
            changed = true;
        }
        const PrCssKeyframeEffect* effect = animation->effect;
        if (effect == nullptr || effect->target == nullptr)
            continue;
        // A compositor shows a composited effect while it runs, and the target is visited when its box stops being a
        // layer.
        bool visit = animation->composited ? (changed || moves) && !compositedNow(presentation, effect) : changed || moves;
        if (visit)
            cssPresentationAnimateTarget(presentation, effect->target, effect->pseudo);
    }
    // web-animations-1/#removing-replaced-animations: a finished animation that a script made, whose every property a
    // later one of the same target fills, is removed.
    for (uint32_t t = 0; t < a.targets.count; ++t) {
        RawArray<PrCssKeyframeEffect*> stack;
        stack.append(a.targets.data[t].effects.data, a.targets.data[t].effects.count);
        sortItems(stack.span(), [](PrCssKeyframeEffect* x, PrCssKeyframeEffect* y) {
            return cssAnimationBefore(x->animation, y->animation);
        });
        auto replaceable = [](const PrCssAnimation* animation) {
            return animation->owner == nullptr && animation->replaceState == PrCssReplaceState_Active &&
                animation->timeline != nullptr && prCssAnimationPlayState(animation) == PrCssPlayState_Finished &&
                animation->effect != nullptr && !prF64IsNan(cssAnimationComputedTiming(animation).activeTime);
        };
        for (uint32_t i = 0; i < stack.count; ++i) {
            PrCssAnimation* animation = stack.data[i]->animation;
            if (!replaceable(animation) || animation->effect->frames.count == 0)
                continue;
            bool covered = true;
            for (uint32_t k = 0; k < animation->effect->frames.count && covered; ++k) {
                PrCssProperty property = animation->effect->frames.data[k].property;
                bool later = false;
                for (uint32_t j = i + 1; j < stack.count && !later; ++j) {
                    const PrCssKeyframeEffect* above = stack.data[j];
                    if (!replaceable(above->animation) && above->animation->owner == nullptr &&
                            prF64IsNan(cssAnimationComputedTiming(above->animation).activeTime))
                        continue;
                    for (uint32_t m = 0; m < above->frames.count && !later; ++m)
                        later = above->frames.data[m].property == property;
                }
                covered = later;
            }
            if (!covered)
                continue;
            animation->replaceState = PrCssReplaceState_Removed;
            double now = prCssTimelineCurrentTime(animation->timeline);
            cssAnimationQueuePlaybackEvent(animation, "remove", cssAnimationCurrentTime(animation),
                prF64IsNan(now) ? kPrF64Nan : now + animation->timeline->originTime);
            cssPresentationAnimateTarget(presentation, animation->effect->target, animation->effect->pseudo);
        }
        stack.reset();
    }
    for (uint32_t i = 0; i < list.count; ++i)
        findCssEvents(list.data[i]);
    for (uint32_t i = 0; i < list.count; ++i)
        prCssAnimationRelease(list.data[i]);
    list.reset();
    cssAnimationUnlistIdle(a);
    // A driver may add or remove drivers while it updates. An added driver first updates in the next update. A driver
    // removed before its turn does not update.
    RawArray<CssAnimationDriver> drivers;
    drivers.append(a.drivers.span());
    for (uint32_t i = 0; i < drivers.count; ++i) {
        bool registered = false;
        for (uint32_t j = 0; j < a.drivers.count && !registered; ++j)
            registered = a.drivers.data[j].context == drivers.data[i].context;
        if (registered)
            drivers.data[i].update(drivers.data[i].context, time);
    }
    drivers.reset();
}

void cssAnimateRendered(PrCssPresentation* presentation) {
    CssAnimations& a = animationsOf(presentation);
    ++a.renders;
    a.renderTime = a.time;
}

void cssAnimateQueueCancelEvent(PrCssAnimation* animation) {
    // A CSS animation or transition that its style owns, and whose effect had not ended, reports its cancellation.
    if (animation->owner == nullptr || animation->effect == nullptr)
        return;
    AnimationPhase phase = animation->eventPhase;
    if (phase == AnimationPhase_Idle || phase == AnimationPhase_After)
        return;
    double current = cssAnimationCurrentTime(animation);
    const PrLayoutEffectTiming& t = animation->effect->timing;
    double active = cssAnimationComputedTiming(animation).activeDuration;
    double elapsed = prF64IsNan(current) ? 0 : prF64Max(prF64Min(current - t.delay, active), 0);
    double now = animation->timeline != nullptr ? prCssTimelineCurrentTime(animation->timeline) : kPrF64Nan;
    PrStringView type = animation->kind == PrCssAnimationKind_Transition ? "transitioncancel" : "animationcancel";
    queueCssEvent(animation, type, elapsed, prF64IsNan(now) ? kPrF64Nan : now + animation->timeline->originTime);
    animation->eventPhase = AnimationPhase_Idle;
}

RawArray<CssAnimateEvent> cssAnimateTakeEvents(PrDomDocument* document) {
    CssAnimations& a = cssAnimations(document);
    RawArray<CssAnimateEvent> events = a.events;
    a.events = {};
    // web-animations-1/#update-animations-and-send-events: by scheduled time, those with none first, and then in
    // composite order.
    stableSortItems(events.span(), [](const CssAnimateEvent& x, const CssAnimateEvent& y) {
        bool xNone = prF64IsNan(x.scheduled);
        bool yNone = prF64IsNan(y.scheduled);
        if (xNone != yNone)
            return xNone;
        if (!xNone && x.scheduled != y.scheduled)
            return x.scheduled < y.scheduled;
        return x.animation != y.animation && cssAnimationBefore(x.animation, y.animation);
    });
    return events;
}

void cssAnimateEventReset(CssAnimateEvent* event) {
    prRawStringReset(&event->name);
    if (event->kind == PrDomEventKind_AnimationPlayback)
        prCssAnimationRelease(static_cast<PrCssAnimation*>(event->target));
    else
        domNodeUnhold(static_cast<PrDomElement*>(event->target));
    prCssAnimationRelease(event->animation);
}

bool cssAnimateHaveEventsAhead(PrDomDocument* document) {
    const CssAnimations& a = cssAnimations(document);
    for (uint32_t i = 0; i < a.list.count; ++i) {
        const PrCssAnimation* animation = a.list.data[i];
        bool timed = animation->timeline != nullptr && !prF64IsNan(prCssTimelineCurrentTime(animation->timeline));
        if ((animation->pending != CssPendingTask_None && timed) || animation->finishScheduled)
            return true;
        // A running animation has its end ahead, unless it has none. The iterations of an infinite CSS animation do not
        // count, so a page with one can be idle once it has started.
        if (prCssAnimationPlayState(animation) != PrCssPlayState_Running || animation->effect == nullptr)
            continue;
        PrLayoutComputedTiming timing = cssAnimationComputedTiming(animation);
        if (timing.phase == PrLayoutEffectPhase_Before || prF64IsFinite(timing.endTime))
            return true;
    }
    for (uint32_t i = 0; i < a.drivers.count; ++i) {
        if (a.drivers.data[i].nextEventTime(a.drivers.data[i].context, a.time) >= 0)
            return true;
    }
    return false;
}

double cssAnimateNextEventTime(PrDomDocument* document) {
    const CssAnimations& a = cssAnimations(document);
    double time = a.time;
    double next = -1;
    auto ahead = [&](double at) {
        if (at > time && (next < 0 || at < next))
            next = at;
    };
    for (uint32_t i = 0; i < a.list.count; ++i) {
        const PrCssAnimation* animation = a.list.data[i];
        bool timed = animation->timeline != nullptr && !prF64IsNan(prCssTimelineCurrentTime(animation->timeline));
        if (animation->pending != CssPendingTask_None && timed)
            return time;
        if (animation->effect == nullptr || !prLayoutPlaybackMoves(&animation->playback) ||
                prF64IsNan(animation->playback.startTime))
            continue;
        if (animation->owner != nullptr && eventPhaseOf(animation) != animation->eventPhase)
            return time;
        // The boundaries of the effect's phases and iterations, and its end, on the document's timeline.
        const PrLayoutEffectTiming& t = animation->effect->timing;
        PrLayoutComputedTiming computed = cssAnimationComputedTiming(animation);
        double origin = animation->timeline != nullptr ? animation->timeline->originTime : 0;
        auto at = [&](double local) {
            return animation->playback.startTime + local / animation->playback.rate + origin;
        };
        ahead(at(t.delay));
        ahead(at(t.delay + computed.activeDuration));
        ahead(at(computed.endTime));
        if (computed.phase == PrLayoutEffectPhase_Active && t.duration > 0 && prF64IsFinite(computed.currentIteration)) {
            double boundary = (computed.currentIteration + (animation->playback.rate > 0 ? 1 : 0) - t.iterationStart);
            ahead(at(t.delay + boundary * t.duration));
        }
    }
    for (uint32_t i = 0; i < a.drivers.count; ++i) {
        double at = a.drivers.data[i].nextEventTime(a.drivers.data[i].context, time);
        if (at == time)
            return time;
        ahead(at);
    }
    return next;
}

bool cssAnimateIsAnimating(PrDomDocument* document) {
    const CssAnimations& a = cssAnimations(document);
    for (uint32_t i = 0; i < a.list.count; ++i) {
        const PrCssAnimation* animation = a.list.data[i];
        bool timed = animation->timeline != nullptr && !prF64IsNan(prCssTimelineCurrentTime(animation->timeline));
        if (animation->pending != CssPendingTask_None && timed)
            return true;
        if (animation->effect != nullptr && prLayoutPlaybackMoves(&animation->playback) &&
                prCssAnimationPlayState(animation) == PrCssPlayState_Running)
            return true;
    }
    for (uint32_t i = 0; i < a.drivers.count; ++i) {
        if (a.drivers.data[i].animating(a.drivers.data[i].context))
            return true;
    }
    return false;
}

} // namespace pr_impl
