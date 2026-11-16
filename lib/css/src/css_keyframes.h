// Keyframes: the values that a keyframe effect moves through, whether a @keyframes rule gives them or a script does.
//
// css-animations-1 - https://drafts.csswg.org/css-animations-1/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
//
// A set of keyframes is the one form that every keyframe effect computes its values from, as a presentation styles the
// effect's target (cssAnimateRestyled). A @keyframes rule compiles into one as its rule set is built. A script's
// keyframes build one from their parsed declarations (web-animations-1/#processing-a-keyframes-argument). Each keyframe
// holds its declarations as a block of longhands, and the offset, easing and composite operation that apply to them.

#pragma once

#include "pr/css_animations.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

#include "css/src/css_animation.h"
#include "css/src/css_rule.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the operation that a keyframe's composite gives, with `effect` standing in for Auto.
PrCssComposite cssKeyframeComposite(PrCssComposite composite, PrCssComposite effect);

// A property of a script's keyframe as the script gave it, which getKeyframes reports.
struct CssKeyframeGiven {
    PrRawString name;
    PrRawString value;
};

// A keyframe (web-animations-1/#keyframe). A keyframe of a @keyframes rule refers to the declarations of its keyframe
// rule, which it holds a reference to. A script's keyframe owns its declarations, and keeps its properties as given.
struct CssKeyframeRecord {
    double offset;                      // as given, in [0, 1], or NaN for a script keyframe that gives none
    double computedOffset;              // the offset, or the offset that spacing gives a keyframe without one
    const CssDeclarationBlock* style;   // longhands, and the animation properties that a keyframe rule may declare
    CssValueList easing;                // the keyframe's easing, which applies from it to the next, or empty for linear
    PrCssComposite composite;
    const CssRuleData* PR_NULLABLE rule; // the keyframe rule that holds `style`, or null if `owned` does
    CssDeclarationBlock owned;
    RawArray<CssKeyframeGiven> given;
};

// A keyframe effect's keyframes, in the order given, which getKeyframes reports
// (web-animations-1/#dom-keyframeeffect-getkeyframes). The effect sorts them by computed offset as it computes their
// values, keeping that order among equal offsets.
struct CssKeyframes {
    uint32_t references;
    RawArray<CssKeyframeRecord> frames;
};

// Compiles a @keyframes rule into keyframes, with one reference that the caller holds. A keyframe rule with several
// selectors gives a keyframe for each, at its offset. A keyframe's easing and composite operation are the
// animation-timing-function and animation-composition that it declares (css-animations-1/#keyframes,
// css-animations-2/#animation-composition). A selector outside [0%, 100%] gives no keyframe.
CssKeyframes* cssKeyframesCompile(const CssRuleData* rule);

// What a script gives for one keyframe (web-animations-1/#processing-a-keyframes-argument): its offset, or NaN for
// none, its easing and composite operation, and its properties, each by its CSS name with its value as written.
struct CssKeyframeInput {
    double offset;
    double computedOffset;              // NaN to space the keyframe between those around it
    PrStringView easing;                // the text of an <easing-function>, or the null view for linear
    PrCssComposite composite;
    const PrCssKeyframeProperty* PR_NULLABLE properties;
    uint32_t propertyCount;
};

// Why script keyframes are refused (web-animations-1/#processing-a-keyframes-argument).
enum {
    CssKeyframesError_None,
    CssKeyframesError_Offset,           // an offset is outside [0, 1], or the offsets decrease
    CssKeyframesError_Easing,           // an easing is not an <easing-function>
};
typedef uint8_t CssKeyframesError;

// Makes keyframes from a script's, with one reference that the caller holds, and spaces the keyframes without offsets
// between those with them (web-animations-1/#compute-missing-keyframe-offsets). Writes null, and returns why, if the
// keyframes are refused.
CssKeyframesError cssKeyframesFromInput(const CssKeyframeInput* inputs, uint32_t count,
    CssKeyframes* PR_NULLABLE* PR_NONNULL out);

// Adds or releases a reference. The last release frees the keyframes and releases the rules they refer to.
void cssKeyframesRetain(CssKeyframes* keyframes);
void cssKeyframesRelease(CssKeyframes* PR_NULLABLE keyframes);

// Returns true if any keyframe declares `property`.
bool cssKeyframesDeclare(const CssKeyframes* keyframes, PrCssProperty property);

// Parses `text` as one <easing-function> into `out`. Returns false, leaving `out` unchanged, if it is not one.
bool cssParseEasing(PrStringView text, CssValueList* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
