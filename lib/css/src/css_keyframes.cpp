// Keyframes: the values that a keyframe effect moves through, whether a @keyframes rule gives them or a script does.
//
// css-animations-1 - https://drafts.csswg.org/css-animations-1/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/

#include "css/src/css_keyframes.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/text_buffer.h"

#include "css/src/css_parse.h"

namespace pr_impl {

namespace {

// Returns the value that the block declares for `property`, or an empty run if it declares none.
CssValueRun declared(const CssDeclarationBlock* block, PrCssProperty property) {
    const CssDeclaration* declarations = cssDeclarations(block);
    for (uint32_t i = block->count; i-- != 0;) {
        if (declarations[i].property == property)
            return cssDeclarationValue(block, &declarations[i]);
    }
    return CssValueRun{};
}

CssKeyframes* newKeyframes() {
    CssKeyframes* keyframes = allocZeroed<CssKeyframes>();
    keyframes->references = 1;
    return keyframes;
}

} // namespace

PrCssComposite cssKeyframeComposite(PrCssComposite composite, PrCssComposite effect) {
    switch (composite) {
        case PrCssComposite_Replace: return PrCssComposite_Replace;
        case PrCssComposite_Add: return PrCssComposite_Add;
        case PrCssComposite_Accumulate: return PrCssComposite_Accumulate;
        default: return effect;
    }
}

CssKeyframes* cssKeyframesCompile(const CssRuleData* rule) {
    CssKeyframes* keyframes = newKeyframes();
    for (uint32_t c = 0; c < rule->children.count; ++c) {
        const CssRuleData* frame = rule->children.data[c];
        if (frame->kind != CssRuleDataKind_Keyframe)
            continue;
        const CssDeclarationBlock* block = &frame->style;
        CssValueRun easing = declared(block, PrCssProperty_AnimationTimingFunction);
        CssValueRun composite = declared(block, PrCssProperty_AnimationComposition);
        // The selectors are the percentages and keywords anywhere in the prelude's productions.
        CssValueRun offsets = cssValueListRun(&frame->prelude);
        for (uint32_t i = 0; i < offsets.count; ++i) {
            const CssValueNode* offset = &offsets.values[i];
            double at = -1;
            if (offset->kind == PrCssValueKind_Percentage)
                at = offset->number / 100;
            else if (cssValueIsKeyword(offset, PrCssKeyword_From))
                at = 0;
            else if (cssValueIsKeyword(offset, PrCssKeyword_To))
                at = 1;
            if (!(at >= 0 && at <= 1))
                continue;
            CssKeyframeRecord& made = keyframes->frames.emplace();
            made = CssKeyframeRecord{};
            made.offset = at;
            made.computedOffset = at;
            made.style = block;
            made.easing = easing.count != 0 ? cssValueListCopy(easing) : CssValueList{};
            made.composite = composite.count != 0 ? cssCompositeFromValue(composite) : PrCssComposite_Auto;
            cssRuleDataRetain(const_cast<CssRuleData*>(frame));
            made.rule = frame;
        }
    }
    return keyframes;
}

CssKeyframesError cssKeyframesFromInput(const CssKeyframeInput* inputs, uint32_t count,
        CssKeyframes* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    // The given offsets are each in [0, 1], and in order.
    double last = 0;
    for (uint32_t i = 0; i < count; ++i) {
        double offset = inputs[i].offset;
        if (prF64IsNan(offset))
            continue;
        if (!(offset >= 0 && offset <= 1) || offset < last)
            return CssKeyframesError_Offset;
        last = offset;
    }
    CssKeyframes* keyframes = newKeyframes();
    for (uint32_t i = 0; i < count; ++i) {
        const CssKeyframeInput& input = inputs[i];
        CssKeyframeRecord& made = keyframes->frames.emplace();
        made = CssKeyframeRecord{};
        made.offset = input.offset;
        made.composite = input.composite;
        if (input.easing.data != nullptr && !cssParseEasing(input.easing, &made.easing)) {
            cssKeyframesRelease(keyframes);
            return CssKeyframesError_Easing;
        }
        // The properties parse as one block, in which a value that does not parse is dropped as it would be in a style
        // attribute. A later property of a keyframe wins over an earlier one of the same longhand.
        PrTextBuffer text{};
        for (uint32_t k = 0; k < input.propertyCount; ++k) {
            const PrCssKeyframeProperty& property = input.properties[k];
            made.given.push(CssKeyframeGiven{prRawStringCopy(property.name), prRawStringCopy(property.value)});
            prTextBufferAppend(&text, property.name);
            prTextBufferAppend(&text, ":");
            prTextBufferAppend(&text, property.value);
            prTextBufferAppend(&text, ";");
        }
        cssDeclarationBlockParse(&made.owned, prTextBufferView(&text), nullptr, CssOrigin_Author);
        prTextBufferReset(&text);
    }
    // The blocks are in place once the array no longer grows.
    for (uint32_t i = 0; i < keyframes->frames.count; ++i)
        keyframes->frames.data[i].style = &keyframes->frames.data[i].owned;

    // web-animations-1/#compute-missing-keyframe-offsets: the first keyframe is at 0 and the last at 1 unless they say
    // otherwise, and the keyframes between two with offsets are spaced evenly between them.
    CssKeyframeRecord* frames = keyframes->frames.data;
    for (uint32_t i = 0; i < count; ++i)
        frames[i].computedOffset = !prF64IsNan(inputs[i].computedOffset) ? inputs[i].computedOffset : frames[i].offset;
    if (count > 1 && prF64IsNan(frames[0].computedOffset))
        frames[0].computedOffset = 0;
    if (count > 0 && prF64IsNan(frames[count - 1].computedOffset))
        frames[count - 1].computedOffset = 1;
    uint32_t previous = 0;
    for (uint32_t i = 1; i < count; ++i) {
        if (prF64IsNan(frames[i].computedOffset))
            continue;
        double from = frames[previous].computedOffset;
        double to = frames[i].computedOffset;
        for (uint32_t k = previous + 1; k < i; ++k)
            frames[k].computedOffset = from + (to - from) * double(k - previous) / double(i - previous);
        previous = i;
    }
    *out = keyframes;
    return CssKeyframesError_None;
}

void cssKeyframesRetain(CssKeyframes* keyframes) {
    ++keyframes->references;
}

void cssKeyframesRelease(CssKeyframes* PR_NULLABLE keyframes) {
    if (keyframes == nullptr || --keyframes->references != 0)
        return;
    for (uint32_t i = 0; i < keyframes->frames.count; ++i) {
        CssKeyframeRecord& frame = keyframes->frames.data[i];
        cssValueListReset(&frame.easing);
        cssDeclarationBlockReset(&frame.owned);
        for (uint32_t k = 0; k < frame.given.count; ++k) {
            prRawStringReset(&frame.given.data[k].name);
            prRawStringReset(&frame.given.data[k].value);
        }
        frame.given.reset();
        cssRuleDataRelease(const_cast<CssRuleData*>(frame.rule));
    }
    keyframes->frames.reset();
    prMemoryFree(keyframes);
}

bool cssKeyframesDeclare(const CssKeyframes* keyframes, PrCssProperty property) {
    for (uint32_t i = 0; i < keyframes->frames.count; ++i) {
        const CssDeclarationBlock* block = keyframes->frames.data[i].style;
        const CssDeclaration* declarations = cssDeclarations(block);
        for (uint32_t d = 0; d < block->count; ++d) {
            if (declarations[d].property == property)
                return true;
        }
    }
    return false;
}

bool cssParseEasing(PrStringView text, CssValueList* out) {
    // animation-timing-function takes a list of easing functions, whose single item is one.
    PrTextBuffer declaration{};
    prTextBufferAppend(&declaration, "animation-timing-function:");
    prTextBufferAppend(&declaration, text);
    CssDeclarationBlock block{};
    cssDeclarationBlockParse(&block, prTextBufferView(&declaration), nullptr, CssOrigin_Author);
    prTextBufferReset(&declaration);
    CssValueRun value = declared(&block, PrCssProperty_AnimationTimingFunction);
    bool one = value.count != 0 && cssListItems(value).count == 1;
    // A CSS-wide keyword is a valid declaration but no easing function.
    if (one && value.count == 1 && value.values[0].kind == PrCssValueKind_Keyword &&
            cssIsCssWideKeyword(cssValueKeyword(value.values)))
        one = false;
    if (one)
        *out = cssValueListCopy(value);
    cssDeclarationBlockReset(&block);
    return one;
}

} // namespace pr_impl
