// The animations that a presentation runs. These are the transitions that changes to an element's style start, and the
// CSS animations that its style names. They apply over the element's base style as the animation and transition origins
// (css-cascade-5/#cascade-origin-animation).
//
// css-transitions-1 - https://drafts.csswg.org/css-transitions-1/
// css-animations-1 - https://drafts.csswg.org/css-animations-1/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/

#include "css/src/css_animate.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/string_ops.h"

#include "css/src/css_animation.h"
#include "css/src/css_cascade.h"
#include "css/src/css_keyword.h"
#include "css/src/css_parse.h"
#include "css/src/css_presentation.h"
#include "css/src/css_property.h"
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
    CssStyleGroup_Grid,
};

CssValueRun animationValue(const CssComputedStyle* style, PrCssProperty property) {
    return cssComputedValue(style, property);
}

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
    return target->type == DomTargetType_PseudoElement ? static_cast<const CssPresentationPseudo*>(target)->pseudo
                                                       : CssPseudo_None;
}

bool displaysNone(const CssComputedStyle* style) {
    CssValueRun display = cssComputedValue(style, PrCssProperty_Display);
    for (uint32_t i = 0; i < display.count; ++i) {
        if (cssValueIsKeyword(&display.values[i], PrCssKeyword_None))
            return true;
    }
    return false;
}

void resetTransition(CssRunningTransition& transition) {
    cssValueListReset(&transition.easing);
    cssValueListReset(&transition.from);
    cssValueListReset(&transition.to);
}

void resetAnimation(CssRunningAnimation& animation) {
    prStringReset(&animation.name);
    cssValueListReset(&animation.easing);
    cssValuePoolReset(&animation.values);
    animation.keyframes.reset();
}

// Returns the index of the target's transition of `property`, or the number of transitions if it has none.
uint32_t transitionOf(const CssAnimateState& a, const PrDomNode* target, PrCssProperty property) {
    for (uint32_t i = 0; i < a.transitions.count; ++i) {
        if (a.transitions.data[i].target == target && a.transitions.data[i].property == property)
            return i;
    }
    return a.transitions.count;
}

// Returns the phase of a transition at `time`.
AnimationPhase transitionPhase(const CssRunningTransition& t, double time) {
    double local = time - t.start - t.delay;
    if (local < 0)
        return AnimationPhase_Before;
    return local < t.duration ? AnimationPhase_Active : AnimationPhase_After;
}

// Returns the duration of an animation's active phase, which is infinite for infinite iterations.
double activeDuration(const PrLayoutTiming& timing) {
    return timing.duration <= 0 || timing.iterations <= 0 ? 0 : timing.duration * timing.iterations;
}

// Returns the phase of a CSS animation at `time`. In the active phase, also writes the iteration that it is in.
AnimationPhase animationPhase(const PrLayoutTiming& timing, double time, double* iteration) {
    double since = (timing.paused ? timing.hold : time - timing.start) - timing.delay;
    *iteration = 0;
    if (since < 0)
        return AnimationPhase_Before;
    if (since >= activeDuration(timing))
        return AnimationPhase_After;
    *iteration = timing.duration > 0 ? floorF64(since / timing.duration) : 0;
    return AnimationPhase_Active;
}

// Returns the time in the active phase from which an animation's events measure. A negative delay moves this time
// forward from the start of the active phase.
double intervalStart(double delay, double active) {
    return maxF64(minF64(-delay, active), 0);
}

// Queues an event of an animation of `target` for the next rendering update, if a window fires the presentation's
// events.
void queueEvent(CssAnimateState& a, const PrDomNode* target, PrStringView type, PrStringView name,
                double elapsedMilliseconds, double scheduled, bool transition) {
    if (!a.recordsEvents)
        return;
    const PrDomElement* element = originOf(target);
    domNodeHold(const_cast<PrDomElement*>(element));
    a.events.push(CssAnimateEvent{element, pseudoOf(target), type, prStringCopy(name),
                                       elapsedMilliseconds / 1000, scheduled, transition});
}

// Queues transitioncancel for a transition that is canceled before it ends.
void cancelTransition(CssAnimateState& a, const CssRunningTransition& t) {
    if (t.phase == AnimationPhase_Idle || transitionPhase(t, a.time) == AnimationPhase_After)
        return;
    double elapsed = maxF64(0, minF64(a.time - t.start - t.delay, t.duration));
    queueEvent(a, t.target, "transitioncancel", cssPropertyName(t.property), elapsed, a.time, true);
}

// Queues animationcancel for a CSS animation that is canceled before it ends.
void cancelAnimation(CssAnimateState& a, const CssRunningAnimation& animation) {
    double iteration = 0;
    if (animation.phase == AnimationPhase_Idle || animation.phase == AnimationPhase_After
        || animationPhase(animation.timing, a.time, &iteration) == AnimationPhase_After)
        return;
    const PrLayoutTiming& timing = animation.timing;
    double since = (timing.paused ? timing.hold : a.time - timing.start) - timing.delay;
    double elapsed = maxF64(0, minF64(since, activeDuration(timing)));
    queueEvent(a, animation.target, "animationcancel", prStringView(&animation.name), elapsed, a.time, false);
}

// Removes a transition, and cancels it if it has not ended.
void removeTransition(CssAnimateState& a, uint32_t index) {
    cancelTransition(a, a.transitions.data[index]);
    resetTransition(a.transitions.data[index]);
    a.transitions.erase(index);
}

// Returns the eased progress of a transition at `time`, with the backwards fill that transitions have
// (css-transitions-1/#application).
double transitionProgress(const CssRunningTransition& t, double time) {
    double local = time - t.start - t.delay;
    if (local < 0)
        return 0;
    if (t.duration <= 0 || local >= t.duration)
        return 1;
    PrLayoutEasing easing = cssEasing(listRun(t.easing));
    return prLayoutEase(&easing, local / t.duration, false);
}

// Appends a transition's value at `time`.
void addTransitionValue(CssValuePool* out, const CssRunningTransition& t, double time, const CssColorContext& colors) {
    cssInterpolate(out, t.property, listRun(t.from), listRun(t.to), transitionProgress(t, time), colors);
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
        PrStringView name = value->kind == PrCssValueKind_Keyword ? cssKeywordName(cssValueKeyword(value))
                            : value->kind == PrCssValueKind_Ident ? cssValueText(item, value)
                                                                : PrStringView{};
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
    context.parentColor =
        c.parent != nullptr ? cssStyleColor(c.parent, kCssDefaultColorEnvironment) : PrColor{0, 0, 0, 1};
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
        CssCascadeInput input{.element = originOf(context.target), .parent = context.animation.parent,
                              .rootFont = context.animation.rootFont, .viewport = context.animation.viewport,
                              .fresh = true, .matched = nullptr, .pseudo = pseudoOf(context.target),
                              .userAgentOnly = true};
        context.reverted =
            cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
    }
    return context.reverted;
}

// Computes the keyframes of `rule` for the target into the animation (css-animations-1/#keyframes). A keyframe that
// declares no animation-composition takes `composite`, the animation's (css-animations-2/#animation-composition).
void computeKeyframes(CssRunningAnimation& animation, const CssRuleData* rule, KeyframeContext& context,
                      CssComposite composite) {
    cssValuePoolReset(&animation.values);
    animation.keyframes.clear();
    animation.inheritsReset = false;
    for (uint32_t c = 0; c < rule->children.count; ++c) {
        const CssRuleData* frame = rule->children.data[c];
        if (frame->kind != CssRuleDataKind_Keyframe)
            continue;
        const CssDeclarationBlock* block = &frame->style;
        const CssDeclaration* declarations = cssDeclarations(block);
        // The keyframe's own easing, which applies from it to the next.
        CssValueSpan easing{0, 0};
        CssComposite frameComposite = composite;
        for (uint32_t d = 0; d < block->count; ++d) {
            if (declarations[d].property == PrCssProperty_AnimationTimingFunction)
                easing = cssValuePoolAddRun(&animation.values, cssDeclarationValue(block, &declarations[d]));
            if (declarations[d].property == PrCssProperty_AnimationComposition)
                frameComposite = cssCompositeFromValue(cssDeclarationValue(block, &declarations[d]));
        }
        CssValueRun offsets = cssValueListRun(&frame->prelude);
        // Adds the keyframe's value `run` of the longhand `property`.
        auto addValue = [&](PrCssProperty property, CssValueRun run) {
            CssStyleSlot slot = cssStyleSlot(property);
            if (slot.group == CssStyleGroup_Count || slot.group == CssStyleGroup_Animation)
                return;
            uint32_t start = animation.values.values.count;
            bool base = false;
            if (run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword
                && cssIsCssWideKeyword(cssValueKeyword(run.values))) {
                // initial, inherit and unset give the values that they give in a declaration. revert gives the user
                // agent's value, and revert-layer and revert-rule the base value.
                PrCssKeyword keyword = cssValueKeyword(run.values);
                bool inherits = keyword == PrCssKeyword_Inherit
                                || (keyword == PrCssKeyword_Unset && bitsetTest(kCssPropertyInherited, property));
                if (keyword == PrCssKeyword_RevertLayer || keyword == PrCssKeyword_RevertRule) {
                    base = true;
                } else if (keyword == PrCssKeyword_Revert) {
                    cssValuePoolAddRun(&animation.values, cssComputedValue(revertedStyle(context), property));
                } else if (inherits && context.parent != nullptr) {
                    cssValuePoolAddRun(&animation.values, cssComputedValue(context.parent, property));
                    animation.inheritsReset = animation.inheritsReset || !bitsetTest(kCssPropertyInherited, property);
                } else
                    cssStyleComputeInitial(&animation.values, property, context.of(property));
            } else {
                cssStyleComputeValue(&animation.values, property, run, context.of(property));
            }
            CssValueSpan value{start, animation.values.values.count - start};
            // The selectors are the percentages and keywords anywhere in the prelude's productions.
            for (uint32_t i = 0; i < offsets.count; ++i) {
                const CssValueNode* offset = &offsets.values[i];
                double at = -1;
                if (offset->kind == PrCssValueKind_Percentage)
                    at = offset->number / 100;
                else if (cssValueIsKeyword(offset, PrCssKeyword_From))
                    at = 0;
                else if (cssValueIsKeyword(offset, PrCssKeyword_To))
                    at = 1;
                if (at >= 0 && at <= 1)
                    animation.keyframes.push(CssAnimateKeyframe{at, property, value, easing, frameComposite, base});
            }
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
            } else if (run.count == 1 && run.values[0].kind == PrCssValueKind_Keyword
                       && cssIsCssWideKeyword(cssValueKeyword(run.values))) {
                for (uint32_t l = 0; l < longhands.count; ++l)
                    addValue(kCssPropertyRuns[longhands.start + l], run);
            }
        }
    }
    // Insertion keeps keyframes of equal offsets in their order.
    for (uint32_t i = 1; i < animation.keyframes.count; ++i) {
        CssAnimateKeyframe moving = animation.keyframes.data[i];
        uint32_t k = i;
        for (; k != 0 && animation.keyframes.data[k - 1].offset > moving.offset; --k)
            animation.keyframes.data[k] = animation.keyframes.data[k - 1];
        animation.keyframes.data[k] = moving;
    }
}

// Appends the animation's value of `property` at `progress`, using `base` at the offsets that no keyframe gives
// (css-animations-1/#keyframes). A keyframe that adds or accumulates is composited onto `base` before the keyframes
// interpolate (web-animations-1/#the-effect-value-of-a-keyframe-animation-effect). Returns false if no keyframe gives
// the property.
bool addAnimationValue(CssValuePool* out, const CssRunningAnimation& a, PrCssProperty property, double progress,
                       CssValueRun base, const CssColorContext& colors) {
    // The property's keyframes, with the implicit ones at 0 and 1. A composited value is a span of `composited`, and
    // its run is read once every value is in place.
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
    for (uint32_t i = 0; i < a.keyframes.count && count + 1 < 32; ++i) {
        const CssAnimateKeyframe& k = a.keyframes.data[i];
        if (k.property != property)
            continue;
        if (!any && k.offset > 0)
            frames[count++] = Frame{0, base, none, {}, false};
        any = true;
        Frame frame{k.offset, k.base ? base : cssValuePoolRun(&a.values, k.value), cssValuePoolRun(&a.values, k.easing),
                    {}, false};
        if (k.composite != CssComposite_Replace) {
            uint32_t start = composited.values.count;
            cssComposite(&composited, property, base, frame.value, k.composite, colors);
            frame.composited = CssValueSpan{start, composited.values.count - start};
            frame.pooled = true;
        }
        frames[count++] = frame;
    }
    if (!any)
        return false;
    if (frames[count - 1].offset < 1)
        frames[count++] = Frame{1, base, none, {}, false};
    for (uint32_t i = 0; i < count; ++i) {
        if (frames[i].pooled)
            frames[i].value = cssValuePoolRun(&composited, frames[i].composited);
    }
    uint32_t at = 0;
    while (at + 1 < count && frames[at + 1].offset <= progress)
        ++at;
    if (at + 1 == count) {
        cssValuePoolAddRun(out, frames[at].value);
    } else {
        const Frame& from = frames[at];
        const Frame& to = frames[at + 1];
        double span = to.offset - from.offset;
        double local = span > 0 ? (progress - from.offset) / span : 1;
        CssValueRun easing = from.easing.count != 0 ? from.easing : listRun(a.easing);
        PrLayoutEasing eased = cssEasing(easing);
        cssInterpolate(out, property, from.value, to.value, prLayoutEase(&eased, local, false), colors);
    }
    cssValuePoolReset(&composited);
    return true;
}

// Records that the property's value is the last `count` values of the scratch pool, replacing an earlier override.
void override(CssAnimateState& a, PrCssProperty property, CssValueSpan span) {
    for (uint32_t i = 0; i < a.overridden.count; ++i) {
        if (a.overridden.data[i] == property) {
            a.overrides.data[i] = span;
            return;
        }
    }
    a.overridden.push(property);
    a.overrides.push(span);
}

// Returns true if a compositor can run an animation of the target's opacity or transform, which it shows on the
// target's box as a layer. An element of SVG content has no box: the content of an svg element draws its children. The
// parent is the flat tree's, so an instance in a use element's shadow tree is SVG content of the use.
bool composites(const PrDomNode* target) {
    const PrDomElement* element = originOf(target);
    const PrDomElement* parent = domElementFromNode(domFlatParent(element));
    return element->space != PrDomNamespace_Svg || parent == nullptr || parent->space != PrDomNamespace_Svg;
}

} // namespace

void cssAnimateRestyled(PrCssPresentation* presentation, const PrDomNode* target,
                        const CssComputedStyle* PR_NULLABLE oldBase, const CssComputedStyle* PR_NULLABLE old,
                        const CssComputedStyle* base,
                        const CssAnimateContext& context, const CssColorEnvironment& environment) {
    CssAnimateState& a = presentation->animations;
    double time = a.time;
    CssColorContext colors = cssStyleColorContext(base, environment);

    // Transitions (css-transitions-1/#starting).
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
            uint32_t running = transitionOf(a, target, property);
            int32_t item = transitionItem(properties, propertyItems, property);
            if (item < 0 || hidden || oldBase == nullptr) {
                if (running != a.transitions.count)
                    removeTransition(a, running);
                continue;
            }
            CssValueRun end = animationValue(base, property);
            if (running != a.transitions.count && cssValueRunEquals(listRun(a.transitions.data[running].to), end))
                continue;
            // The before-change value is what the running transition shows now, or else the old style's value.
            value.values.count = 0;
            value.text.count = 0;
            CssValueRun start{};
            if (running != a.transitions.count) {
                addTransitionValue(&value, a.transitions.data[running], time, colors);
                start = cssValuePoolRun(&value, CssValueSpan{0, value.values.count});
            } else {
                start = animationValue(old != nullptr ? old : oldBase, property);
            }
            double duration = cssTimeMilliseconds(cssListItem(durations, durationItems, uint32_t(item)), 0);
            double delay = cssTimeMilliseconds(cssListItem(delays, delayItems, uint32_t(item)), 0);
            // A value that interpolates only discretely transitions only if the behavior allows it
            // (css-transitions-2/#transition-behavior-property).
            CssValueRun behavior = cssListItem(behaviors, behaviorItems, uint32_t(item));
            bool discrete = behavior.count == 1 && cssValueIsKeyword(behavior.values, PrCssKeyword_AllowDiscrete);
            bool starts = (duration > 0 ? duration : 0) + delay > 0
                          && (cssInterpolable(property, start, end, colors)
                              || (discrete && !cssValueRunEquals(start, end)));
            if (!starts) {
                if (running != a.transitions.count)
                    removeTransition(a, running);
                continue;
            }
            CssRunningTransition made{};
            made.target = target;
            made.property = property;
            made.start = time;
            made.delay = delay;
            made.duration = duration > 0 ? duration : 0;
            made.easing = cssValueListCopy(cssListItem(easings, easingItems, uint32_t(item)));
            made.from = cssValueListCopy(start);
            made.to = cssValueListCopy(end);
            made.composited =
                (property == PrCssProperty_Opacity || property == PrCssProperty_Transform) && composites(target);
            if (running != a.transitions.count) {
                // A new transition of the property cancels the one running (css-transitions-1/#starting).
                cancelTransition(a, a.transitions.data[running]);
                resetTransition(a.transitions.data[running]);
                a.transitions.data[running] = made;
            } else {
                a.transitions.push(made);
            }
        }
    }
    cssValuePoolReset(&value);

    // CSS animations (css-animations-1/#animations). Each item of animation-name that names keyframes is matched by
    // name to the target's running animations, which keep their start time.
    CssValueRun names = cssComputedValue(base, PrCssProperty_AnimationName);
    CssListItems nameItems = cssListItems(names);
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        if (a.animations.data[i].target == target)
            a.animations.data[i].matched = false;
    }
    KeyframeContext keyframes = keyframeContext(base, context);
    keyframes.presentation = presentation;
    keyframes.target = target;
    for (uint32_t n = 0; n < nameItems.count && !hidden; ++n) {
        PrStringView name = animationName(cssListItem(names, nameItems, n));
        const CssRuleData* rule = name.size != 0 ? cssRuleSetKeyframes(&presentation->rules, name) : nullptr;
        if (rule == nullptr)
            continue;
        uint32_t at = a.animations.count;
        for (uint32_t i = 0; i < a.animations.count && at == a.animations.count; ++i) {
            CssRunningAnimation& candidate = a.animations.data[i];
            if (candidate.target == target && !candidate.matched && prStringView(&candidate.name) == name)
                at = i;
        }
        if (at == a.animations.count) {
            CssRunningAnimation& made = a.animations.emplace();
            made = CssRunningAnimation{};
            made.target = target;
            made.name = prStringCopy(name);
            made.timing = prLayoutTimingDefault();
            made.timing.start = time;
        }
        CssRunningAnimation& animation = a.animations.data[at];
        animation.matched = true;
        auto item = [&](PrCssProperty property) {
            CssValueRun list = cssComputedValue(base, property);
            return cssListItem(list, cssListItems(list), n);
        };
        PrLayoutTiming& timing = animation.timing;
        timing.duration = cssTimeMilliseconds(item(PrCssProperty_AnimationDuration), 0);
        timing.delay = cssTimeMilliseconds(item(PrCssProperty_AnimationDelay), 0);
        CssValueRun count = item(PrCssProperty_AnimationIterationCount);
        timing.iterations = cssValueRunKeyword(count) == PrCssKeyword_Infinite ? kInfinityF64
                            : count.count == 1 && cssValueIsNumeric(count.values[0].kind) ? count.values[0].number
                                                                                         : 1;
        if (timing.iterations < 0)
            timing.iterations = 1;
        switch (cssValueRunKeyword(item(PrCssProperty_AnimationDirection))) {
            case PrCssKeyword_Reverse: timing.direction = PrLayoutPlaybackDirection_Reverse; break;
            case PrCssKeyword_Alternate: timing.direction = PrLayoutPlaybackDirection_Alternate; break;
            case PrCssKeyword_AlternateReverse: timing.direction = PrLayoutPlaybackDirection_AlternateReverse; break;
            default: timing.direction = PrLayoutPlaybackDirection_Normal; break;
        }
        switch (cssValueRunKeyword(item(PrCssProperty_AnimationFillMode))) {
            case PrCssKeyword_Forwards: timing.fill = PrLayoutFill_Forwards; break;
            case PrCssKeyword_Backwards: timing.fill = PrLayoutFill_Backwards; break;
            case PrCssKeyword_Both: timing.fill = PrLayoutFill_Both; break;
            default: timing.fill = PrLayoutFill_None; break;
        }
        bool paused = cssValueRunKeyword(item(PrCssProperty_AnimationPlayState)) == PrCssKeyword_Paused;
        // Pausing holds the time since the start, and resuming moves the start to preserve that time.
        if (paused && !timing.paused)
            timing.hold = time - timing.start;
        else if (!paused && timing.paused)
            timing.start = time - timing.hold;
        timing.paused = paused;
        cssValueListReset(&animation.easing);
        animation.easing = cssValueListCopy(item(PrCssProperty_AnimationTimingFunction));
        computeKeyframes(animation, rule, keyframes, cssCompositeFromValue(item(PrCssProperty_AnimationComposition)));
        animation.composited = animation.keyframes.count != 0 && composites(target);
        // A property that an important declaration wins is not animated (css-cascade-5/#cascade-origin), which a
        // compositor would not know.
        for (uint32_t k = 0; k < animation.keyframes.count; ++k) {
            PrCssProperty property = animation.keyframes.data[k].property;
            animation.composited = animation.composited
                                   && (property == PrCssProperty_Opacity || property == PrCssProperty_Transform)
                                   && !cssStyleIsImportant(base, property);
        }
    }
    cssComputedStyleRelease(keyframes.reverted);
    for (uint32_t i = 0; i < a.animations.count;) {
        CssRunningAnimation& animation = a.animations.data[i];
        if (animation.target == target && !animation.matched) {
            cancelAnimation(a, animation);
            resetAnimation(animation);
            a.animations.erase(i);
            continue;
        }
        ++i;
    }
}

const CssComputedStyle* cssAnimateApply(PrCssPresentation* presentation, const PrDomNode* target,
                                        const CssComputedStyle* base, bool composited,
                                        const CssColorEnvironment& environment) {
    CssAnimateState& a = presentation->animations;
    a.scratch.values.count = 0;
    a.scratch.text.count = 0;
    a.overridden.clear();
    a.overrides.clear();
    CssColorContext colors = cssStyleColorContext(base, environment);
    bool inheritsReset = false;
    // CSS animations apply in the order of their names, with later ones winning, and then transitions apply over them.
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        const CssRunningAnimation& animation = a.animations.data[i];
        if (animation.target != target || (animation.composited && !composited))
            continue;
        inheritsReset = inheritsReset || animation.inheritsReset;
        bool finished = false;
        double progress = prLayoutTimingProgress(&animation.timing, a.time, &finished);
        if (progress < 0)
            continue;
        for (uint32_t k = 0; k < animation.keyframes.count; ++k) {
            PrCssProperty property = animation.keyframes.data[k].property;
            // Important declarations override animations (css-cascade-5/#cascade-origin). Transitions, in an origin
            // above them, apply.
            if (cssStyleIsImportant(base, property))
                continue;
            bool seen = false;
            for (uint32_t j = 0; j < k && !seen; ++j)
                seen = animation.keyframes.data[j].property == property;
            if (seen)
                continue;
            uint32_t start = a.scratch.values.count;
            if (addAnimationValue(&a.scratch, animation, property, progress, cssComputedValue(base, property),
                                  colors))
                override(a, property, CssValueSpan{start, a.scratch.values.count - start});
        }
    }
    for (uint32_t i = 0; i < a.transitions.count;) {
        const CssRunningTransition& t = a.transitions.data[i];
        if (t.target != target) {
            ++i;
            continue;
        }
        // A transition that has ended shows the base value, and is dropped.
        if (a.time >= t.start + t.delay + t.duration) {
            removeTransition(a, i);
            continue;
        }
        if (t.composited && !composited) {
            ++i;
            continue;
        }
        uint32_t start = a.scratch.values.count;
        addTransitionValue(&a.scratch, t, a.time, colors);
        override(a, t.property, CssValueSpan{start, a.scratch.values.count - start});
        ++i;
    }
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
    const CssAnimateState& a = presentation->animations;
    for (uint32_t i = 0; i < a.transitions.count; ++i) {
        if (a.transitions.data[i].target == target && a.transitions.data[i].composited)
            return true;
    }
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        const CssRunningAnimation& animation = a.animations.data[i];
        if (animation.target != target || !animation.composited)
            continue;
        bool finished = false;
        double progress = prLayoutTimingProgress(&animation.timing, a.time, &finished);
        if (progress >= 0 || !finished)
            return true;
    }
    return false;
}

const Array<PrLayoutTrack>& cssAnimateTracks(PrCssPresentation* presentation, AnimationNodeFn layoutNode,
                                             AnimationBaseFn baseOf) {
    CssAnimateState& a = presentation->animations;
    a.tracks.clear();
    a.keyframes.clear();
    // A keyframe of a composited effect, from a computed value. A transform's keyframe holds the base's individual
    // transforms before the animated list, which interpolate as equal functions.
    auto frame = [](double offset, PrCssProperty property, CssValueRun value, PrLayoutEasing easing,
                    const CssComputedStyle* PR_NULLABLE base) {
        PrLayoutKeyframe made{};
        made.offset = offset;
        made.easing = easing;
        made.opacity = value.count == 1 && cssValueIsNumeric(value.values[0].kind) ? float(value.values[0].number) : 1;
        if (property == PrCssProperty_Transform && base != nullptr)
            made.transform = cssTransformFunctions(cssComputedValue(base, PrCssProperty_Translate),
                                                   cssComputedValue(base, PrCssProperty_Rotate),
                                                   cssComputedValue(base, PrCssProperty_Scale), value);
        else if (property == PrCssProperty_Transform)
            made.transform = cssTransformFunctions(CssValueRun{}, CssValueRun{}, CssValueRun{}, value);
        return made;
    };
    // Tracks refer to their keyframes by index until every keyframe is made, because the keyframes move while they are
    // added.
    Array<uint32_t> starts;
    auto add = [&](PrLayoutNode node, PrCssProperty property, const PrLayoutTiming& timing, uint32_t start) {
        PrLayoutTrack track{};
        track.node = node;
        track.effect = property == PrCssProperty_Opacity ? PrLayoutEffect_Opacity : PrLayoutEffect_Transform;
        track.timing = timing;
        track.keyframeCount = a.keyframes.count - start;
        a.tracks.push(track);
        starts.push(start);
    };
    for (uint32_t i = 0; i < a.transitions.count; ++i) {
        const CssRunningTransition& t = a.transitions.data[i];
        PrLayoutNode node = t.composited ? layoutNode(presentation, t.target) : PrLayoutNode(kPrLayoutNodeNone);
        if (node == kPrLayoutNodeNone)
            continue;
        const CssComputedStyle* base = baseOf(presentation, t.target);
        uint32_t start = a.keyframes.count;
        a.keyframes.push(frame(0, t.property, listRun(t.from), cssEasing(listRun(t.easing)), base));
        a.keyframes.push(frame(1, t.property, listRun(t.to), prLayoutEasingLinear(), base));
        // A transition fills backwards through its delay (css-transitions-1/#application).
        PrLayoutTiming timing = prLayoutTimingDefault();
        timing.start = t.start;
        timing.delay = t.delay;
        timing.duration = t.duration;
        timing.fill = PrLayoutFill_Backwards;
        add(node, t.property, timing, start);
    }
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        const CssRunningAnimation& animation = a.animations.data[i];
        PrLayoutNode node =
            animation.composited ? layoutNode(presentation, animation.target) : PrLayoutNode(kPrLayoutNodeNone);
        const CssComputedStyle* base = node != kPrLayoutNodeNone ? baseOf(presentation, animation.target) : nullptr;
        if (base == nullptr)
            continue;
        PrLayoutEasing easing = cssEasing(listRun(animation.easing));
        const PrCssProperty effects[] = {PrCssProperty_Opacity, PrCssProperty_Transform};
        for (PrCssProperty property : effects) {
            uint32_t start = a.keyframes.count;
            CssValueRun underlying = cssComputedValue(base, property);
            for (uint32_t k = 0; k < animation.keyframes.count; ++k) {
                const CssAnimateKeyframe& key = animation.keyframes.data[k];
                if (key.property != property)
                    continue;
                // The base value stands in at the offsets that no keyframe gives.
                if (a.keyframes.count == start && key.offset > 0)
                    a.keyframes.push(frame(0, property, underlying, easing, base));
                CssValueRun own = cssValuePoolRun(&animation.values, key.easing);
                a.keyframes.push(frame(key.offset, property, cssValuePoolRun(&animation.values, key.value),
                                       own.count != 0 ? cssEasing(own) : easing, base));
            }
            if (a.keyframes.count == start)
                continue;
            if (a.keyframes.data[a.keyframes.count - 1].offset < 1)
                a.keyframes.push(frame(1, property, underlying, easing, base));
            add(node, property, animation.timing, start);
        }
    }
    for (uint32_t i = 0; i < a.tracks.count; ++i)
        a.tracks.data[i].keyframes = a.keyframes.data + starts.data[i];
    starts.reset();
    return a.tracks;
}

bool cssAnimateHas(const PrCssPresentation* presentation, const PrDomNode* target) {
    const CssAnimateState& a = presentation->animations;
    for (uint32_t i = 0; i < a.transitions.count; ++i) {
        if (a.transitions.data[i].target == target)
            return true;
    }
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        if (a.animations.data[i].target == target)
            return true;
    }
    return false;
}

void cssAnimateForget(PrCssPresentation* presentation, const PrDomNode* target) {
    CssAnimateState& a = presentation->animations;
    for (uint32_t i = 0; i < a.transitions.count;) {
        if (a.transitions.data[i].target == target)
            removeTransition(a, i);
        else
            ++i;
    }
    for (uint32_t i = 0; i < a.animations.count;) {
        if (a.animations.data[i].target == target) {
            cancelAnimation(a, a.animations.data[i]);
            resetAnimation(a.animations.data[i]);
            a.animations.erase(i);
        } else {
            ++i;
        }
    }
}

void cssAnimateFindEvents(PrCssPresentation* presentation) {
    CssAnimateState& a = presentation->animations;
    for (uint32_t i = 0; i < a.transitions.count; ++i) {
        CssRunningTransition& t = a.transitions.data[i];
        AnimationPhase now = transitionPhase(t, a.time);
        AnimationPhase was = t.phase;
        if (now == was)
            continue;
        PrStringView name = cssPropertyName(t.property);
        double from = intervalStart(t.delay, t.duration);
        double start = t.start + t.delay;
        if (was == AnimationPhase_Idle)
            queueEvent(a, t.target, "transitionrun", name, from, t.start, true);
        if ((was == AnimationPhase_Idle || was == AnimationPhase_Before) && now != AnimationPhase_Before)
            queueEvent(a, t.target, "transitionstart", name, from, start + from, true);
        if (now == AnimationPhase_After)
            queueEvent(a, t.target, "transitionend", name, t.duration, start + t.duration, true);
        t.phase = now;
    }
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        CssRunningAnimation& animation = a.animations.data[i];
        double iteration = 0;
        AnimationPhase now = animationPhase(animation.timing, a.time, &iteration);
        AnimationPhase was = animation.phase;
        if (now == was && (now != AnimationPhase_Active || iteration == animation.iteration))
            continue;
        const PrLayoutTiming& timing = animation.timing;
        PrStringView name = prStringView(&animation.name);
        double active = activeDuration(timing);
        double from = intervalStart(timing.delay, active);
        double start = timing.start + timing.delay;
        bool begins = (was == AnimationPhase_Idle || was == AnimationPhase_Before) && now != AnimationPhase_Before;
        if (begins || (was == AnimationPhase_After && now == AnimationPhase_Active))
            queueEvent(a, animation.target, "animationstart", name, from, start + from, false);
        if (was == AnimationPhase_Active && now == AnimationPhase_Active && iteration != animation.iteration)
            queueEvent(a, animation.target, "animationiteration", name, iteration * timing.duration,
                       start + iteration * timing.duration, false);
        if (now == AnimationPhase_After && was != AnimationPhase_After)
            queueEvent(a, animation.target, "animationend", name, active, start + active, false);
        animation.phase = now;
        animation.iteration = iteration;
    }
}

bool cssAnimateHaveEventsAhead(const CssAnimateState* animations) {
    if (animations->transitions.count != 0)
        return true;
    for (uint32_t i = 0; i < animations->animations.count; ++i) {
        const CssRunningAnimation& animation = animations->animations.data[i];
        double iteration = 0;
        if (animation.timing.paused)
            continue;
        // An animation that has not started has its start ahead. A finite animation that has not ended has its end
        // ahead.
        AnimationPhase phase = animationPhase(animation.timing, animations->time, &iteration);
        if (phase == AnimationPhase_Before
            || (isFiniteF64(activeDuration(animation.timing)) && phase != AnimationPhase_After))
            return true;
    }
    return false;
}

double cssAnimateNextEventTime(const CssAnimateState* animations) {
    double time = animations->time;
    double next = -1;
    auto ahead = [&](double at) {
        if (next < 0 || at < next)
            next = at;
    };
    for (uint32_t i = 0; i < animations->transitions.count; ++i) {
        const CssRunningTransition& t = animations->transitions.data[i];
        if (transitionPhase(t, time) != t.phase)
            return time;
        double start = t.start + t.delay;
        if (time < start)
            ahead(start);
        else if (time < start + t.duration)
            ahead(start + t.duration);
    }
    for (uint32_t i = 0; i < animations->animations.count; ++i) {
        const CssRunningAnimation& animation = animations->animations.data[i];
        const PrLayoutTiming& timing = animation.timing;
        double iteration = 0;
        AnimationPhase phase = animationPhase(timing, time, &iteration);
        if (phase != animation.phase || (phase == AnimationPhase_Active && iteration != animation.iteration))
            return time;
        // A paused animation keeps its phase and iteration.
        if (timing.paused)
            continue;
        double start = timing.start + timing.delay;
        double active = activeDuration(timing);
        if (phase == AnimationPhase_Before) {
            ahead(start);
        } else if (phase == AnimationPhase_Active) {
            double end = start + active;
            double turn = timing.duration > 0 ? start + (iteration + 1) * timing.duration : end;
            ahead(minF64(turn, end));
        }
    }
    return next;
}

bool cssAnimationRunning(const CssAnimateState* animations, const CssRunningAnimation* animation) {
    if (animation->timing.paused)
        return false;
    bool finished = false;
    prLayoutTimingProgress(&animation->timing, animations->time, &finished);
    return !finished;
}

void cssAnimateReset(CssAnimateState* animations) {
    for (uint32_t i = 0; i < animations->transitions.count; ++i)
        resetTransition(animations->transitions.data[i]);
    animations->transitions.reset();
    for (uint32_t i = 0; i < animations->animations.count; ++i)
        resetAnimation(animations->animations.data[i]);
    animations->animations.reset();
    cssValuePoolReset(&animations->scratch);
    animations->overridden.reset();
    animations->overrides.reset();
    animations->tracks.reset();
    animations->keyframes.reset();
    for (uint32_t i = 0; i < animations->events.count; ++i) {
        prStringReset(&animations->events.data[i].name);
        domNodeUnhold(const_cast<PrDomElement*>(animations->events.data[i].element));
    }
    animations->events.reset();
}

} // namespace pr_impl
