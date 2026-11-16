// The bindings of Web Animations: AnimationTimeline, DocumentTimeline, AnimationEffect, KeyframeEffect, Animation,
// CSSAnimation, CSSTransition and AnimationPlaybackEvent, and the Animatable and DocumentOrShadowRoot members.
//
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
// web-animations-2 - https://drafts.csswg.org/web-animations-2/
// css-animations-2 - https://drafts.csswg.org/css-animations-2/
// css-transitions-2 - https://drafts.csswg.org/css-transitions-2/

#include "css/quickjs/src/css_animations_quickjs.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/css_animations.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/quickjs_binding.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

namespace {

JSClassID timelineClass;
JSClassID documentTimelineClass;
JSClassID effectClass;
JSClassID keyframeEffectClass;
JSClassID animationClass;
JSClassID cssAnimationClass;
JSClassID cssTransitionClass;
JSClassID playbackEventClass;

constexpr JsReferences kTimelineReferences = {
    [](void* handle) { prCssTimelineRetain(static_cast<PrCssTimeline*>(handle)); },
    [](void* handle) { prCssTimelineRelease(static_cast<PrCssTimeline*>(handle)); },
    false,
};

constexpr JsReferences kEffectReferences = {
    [](void* handle) { prCssKeyframeEffectRetain(static_cast<PrCssKeyframeEffect*>(handle)); },
    [](void* handle) { prCssKeyframeEffectRelease(static_cast<PrCssKeyframeEffect*>(handle)); },
    false,
};

// An animation's object holds a reference to it.
constexpr JsReferences kAnimationReferences = {
    [](void* handle) { prDomEventTargetRetain(static_cast<PrDomEventTarget*>(handle)); },
    [](void* handle) { prDomEventTargetRelease(static_cast<PrDomEventTarget*>(handle)); },
    false,
};

JSValue nullableNumber(JSContext* ctx, double value) {
    return prF64IsNan(value) ? JS_NULL : JS_NewFloat64(ctx, value);
}

// Reads a double (webidl/#js-double), which is finite.
bool readFiniteNumber(JSContext* ctx, JSValueConst value, double* out) {
    if (JS_ToFloat64(ctx, out, value) != 0)
        return false;
    if (!prF64IsFinite(*out)) {
        JS_ThrowTypeError(ctx, "The provided double value is non-finite");
        return false;
    }
    return true;
}

// Reads a nullable double (webidl/#js-nullable-type): null and undefined are NaN, and any other value converts to a
// double, which is finite (webidl/#js-double).
bool readNullableNumber(JSContext* ctx, JSValueConst value, double* out) {
    if (JS_IsNull(value) || JS_IsUndefined(value)) {
        *out = kPrF64Nan;
        return true;
    }
    return readFiniteNumber(ctx, value, out);
}

// Reads a keyframe's offset: null and undefined are NaN, a string is CSS text, a number or a percentage, or a calc()
// that simplifies to one (web-animations-1/#dom-basekeyframe-offset, which level 2 extends to strings), and any other
// value converts to a finite double. Throws a TypeError for text that is not an offset.
bool readOffset(JSContext* ctx, JSValueConst value, double* out) {
    if (!JS_IsString(value))
        return readNullableNumber(ctx, value, out);
    StringArgument text = {ctx};
    if (!text.read(value))
        return false;
    PrCssValue* parsed = prCssValueCreate();
    bool found = false;
    if (prCssValueParseNumeric(text.value(), parsed) == PrStatus_Ok) {
        // A calc() simplifies to one sum of its terms, which is an offset if its only term is a number or a percentage.
        uint32_t terms = 0;
        for (uint32_t node = 0; node < prCssValueSize(parsed); ++node) {
            PrCssValueFields fields = prCssValueFields(parsed, node);
            if (fields.kind == PrCssValueKind_Math)
                continue;
            ++terms;
            bool number = fields.kind == PrCssValueKind_Number || fields.kind == PrCssValueKind_Integer;
            bool percent = fields.kind == PrCssValueKind_Percentage;
            found = number || percent;
            *out = percent ? fields.number.value / 100 : fields.number.value;
        }
        found = found && terms == 1 && prF64IsFinite(*out);
    }
    prCssValueDestroy(parsed);
    if (!found) {
        *out = kPrF64Nan;
        JS_ThrowTypeError(ctx, "Invalid keyframe offset");
    }
    return found;
}

// Reads a member of an enumeration, which throws a TypeError for a string that is not one of `values`.
bool readEnum(JSContext* ctx, JSValueConst value, const char* const* values, uint32_t count, uint32_t* out) {
    PrStringView text{};
    if (!jsReadString(ctx, value, &text))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (text == values[i]) {
            *out = i;
            return true;
        }
    }
    JS_ThrowTypeError(ctx, "The provided value is not a valid enum value");
    return false;
}

constexpr const char* kFills[] = {"none", "forwards", "backwards", "both", "auto"};
constexpr const char* kDirections[] = {"normal", "reverse", "alternate", "alternate-reverse"};
constexpr const char* kComposites[] = {"replace", "add", "accumulate", "auto"};
constexpr const char* kPlayStates[] = {"idle", "running", "paused", "finished"};
constexpr const char* kReplaceStates[] = {"active", "removed", "persisted"};

// Reads an EffectTiming or an OptionalEffectTiming (web-animations-1/#the-effecttiming-dictionaries), whose members are
// read in lexicographic order. `optional` reads the members of OptionalEffectTiming, which have no defaults.
bool readTiming(JSContext* ctx, JSValueConst dictionary, PrCssEffectTiming* out, PrCssTimingMembers* members,
        StringArgument* easing) {
    *members = 0;
    bool present = false;
    if (!jsDictionary(ctx, dictionary, &present))
        return false;
    if (!present)
        return true;
    auto member = [&](const char* name, JSValue* value) {
        *value = JS_GetPropertyStr(ctx, dictionary, name);
        return !JS_IsException(*value);
    };
    JSValue value = JS_UNDEFINED;
    // delay, direction, duration, easing, endDelay, fill, iterationStart, iterations.
    if (!member("delay", &value))
        return false;
    bool ok = true;
    if (!JS_IsUndefined(value)) {
        ok = readFiniteNumber(ctx, value, &out->delay);
        *members |= PrCssTimingMember_Delay;
    }
    JS_FreeValue(ctx, value);
    if (!ok || !member("direction", &value))
        return false;
    if (!JS_IsUndefined(value)) {
        uint32_t direction = 0;
        ok = readEnum(ctx, value, kDirections, countOf(kDirections), &direction);
        out->direction = PrCssPlaybackDirection(direction);
        *members |= PrCssTimingMember_Direction;
    }
    JS_FreeValue(ctx, value);
    if (!ok || !member("duration", &value))
        return false;
    if (!JS_IsUndefined(value)) {
        // (unrestricted double or DOMString): a number, or the string auto.
        if (JS_IsNumber(value)) {
            ok = JS_ToFloat64(ctx, &out->duration, value) == 0;
            if (ok && prF64IsNan(out->duration)) {
                JS_ThrowTypeError(ctx, "The duration is NaN");
                ok = false;
            }
        } else {
            PrStringView text{};
            ok = jsReadString(ctx, value, &text);
            if (ok && text != "auto") {
                JS_ThrowTypeError(ctx, "The duration is not a number or auto");
                ok = false;
            }
            out->duration = kPrF64Nan;
        }
        *members |= PrCssTimingMember_Duration;
    }
    JS_FreeValue(ctx, value);
    if (!ok)
        return false;
    bool given = false;
    if (!jsDictString(ctx, dictionary, "easing", easing, &given))
        return false;
    if (given) {
        out->easing = easing->value();
        *members |= PrCssTimingMember_Easing;
    }
    if (!member("endDelay", &value))
        return false;
    if (!JS_IsUndefined(value)) {
        ok = readFiniteNumber(ctx, value, &out->endDelay);
        *members |= PrCssTimingMember_EndDelay;
    }
    JS_FreeValue(ctx, value);
    if (!ok || !member("fill", &value))
        return false;
    if (!JS_IsUndefined(value)) {
        uint32_t fill = 0;
        ok = readEnum(ctx, value, kFills, countOf(kFills), &fill);
        out->fill = PrCssFillMode(fill);
        *members |= PrCssTimingMember_Fill;
    }
    JS_FreeValue(ctx, value);
    if (!ok || !member("iterationStart", &value))
        return false;
    if (!JS_IsUndefined(value)) {
        ok = readFiniteNumber(ctx, value, &out->iterationStart);
        *members |= PrCssTimingMember_IterationStart;
    }
    JS_FreeValue(ctx, value);
    if (!ok || !member("iterations", &value))
        return false;
    if (!JS_IsUndefined(value)) {
        ok = JS_ToFloat64(ctx, &out->iterations, value) == 0;
        *members |= PrCssTimingMember_Iterations;
    }
    JS_FreeValue(ctx, value);
    return ok;
}

// Throws the TypeError of a refused timing or keyframes, or the SyntaxError of a refused pseudo-element.
JSValue throwStatus(JSContext* ctx, PrStatus status) {
    if (status == PrStatus_Syntax)
        return jsDomExceptionThrow(ctx, "SyntaxError");
    if (status == PrStatus_InvalidState)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return JS_ThrowTypeError(ctx, "The value is not one that the animation accepts");
}

// ---------------------------------------------------------------------------------------------------------------------
// Keyframes (web-animations-1/#processing-a-keyframes-argument)

// Writes the CSS name of the IDL name of an animation property to `out`
// (web-animations-1/#idl-attribute-name-to-animation-property-name). Returns false for a name that is not one: a name
// with a dash other than a custom property's, and float and offset, which are spelled cssFloat and cssOffset.
bool animationPropertyName(PrStringView name, PrTextBuffer* out) {
    prTextBufferClear(out);
    if (name.size >= 2 && name.data[0] == '-' && name.data[1] == '-') {
        prTextBufferAppend(out, name);
        return true;
    }
    if (name == "cssFloat" || name == "cssOffset") {
        prTextBufferAppend(out, name == "cssFloat" ? "float" : "offset");
        return true;
    }
    if (name == "float" || name == "offset")
        return false;
    for (size_t i = 0; i < name.size; ++i) {
        char c = name.data[i];
        if (c == '-')
            return false;
        if (prAsciiIsUpper(c)) {
            prTextBufferAppendChar(out, '-');
            prTextBufferAppendChar(out, prAsciiLower(c));
        } else {
            prTextBufferAppendChar(out, c);
        }
    }
    return true;
}

// Returns the IDL name of a property's CSS name (web-animations-1/#animation-property-name-to-idl-attribute-name).
PrRawString idlPropertyName(PrStringView name) {
    if (name.size >= 2 && name.data[0] == '-' && name.data[1] == '-')
        return prRawStringCopy(name);
    if (name == "float" || name == "offset")
        return prRawStringCopy(name == "float" ? "cssFloat" : "cssOffset");
    PrTextBuffer idl{};
    bool upper = false;
    for (size_t i = 0; i < name.size; ++i) {
        char c = name.data[i];
        if (c == '-') {
            upper = idl.size != 0;
            continue;
        }
        prTextBufferAppendChar(&idl, upper ? prAsciiUpper(c) : c);
        upper = false;
    }
    PrRawString out = prRawStringCopy(prTextBufferView(&idl));
    prTextBufferReset(&idl);
    return out;
}

// A property of a keyframe as it is read.
struct ReadProperty {
    PrRawString name;               // the CSS name
    PrRawString value;
};

// A keyframe as it is read, whose properties are `count` properties of its keyframes from `first`.
struct ReadKeyframe {
    double offset;
    double computedOffset;
    PrRawString easing;
    PrCssComposite composite;
    uint32_t first;
    uint32_t count;
};

// A keyframes argument as it is read, and the form of it that the C interface takes, which refers to it.
struct ReadKeyframes {
    RawArray<ReadKeyframe> frames;
    RawArray<ReadProperty> properties;
    RawArray<PrCssKeyframe> keyframes;
    RawArray<PrCssKeyframeProperty> views;

    ReadKeyframe& add(double computedOffset) {
        ReadKeyframe& frame = frames.emplace();
        frame = ReadKeyframe{
            kPrF64Nan, computedOffset, prRawStringCopy("linear"), PrCssComposite_Auto,
            properties.count, 0,
        };
        return frame;
    }

    // Adds a property to the last keyframe, taking the strings.
    void addProperty(PrRawString name, PrRawString value) {
        properties.push(ReadProperty{name, value});
        ++frames.data[frames.count - 1].count;
    }

    // Returns the C form of the keyframes.
    const PrCssKeyframe* PR_NULLABLE build() {
        views.clear();
        keyframes.clear();
        for (uint32_t i = 0; i < properties.count; ++i)
            views.push(PrCssKeyframeProperty{
                prRawStringView(&properties.data[i].name),
                prRawStringView(&properties.data[i].value),
            });
        for (uint32_t i = 0; i < frames.count; ++i) {
            const ReadKeyframe& frame = frames.data[i];
            keyframes.push(PrCssKeyframe{
                .offset = frame.offset,
                .computedOffset = frame.computedOffset,
                .easing = prRawStringView(&frame.easing),
                .composite = frame.composite,
                .properties = views.data + frame.first,
                .propertyCount = frame.count,
            });
        }
        return keyframes.data;
    }

    void reset() {
        for (uint32_t i = 0; i < frames.count; ++i)
            prRawStringReset(&frames.data[i].easing);
        for (uint32_t i = 0; i < properties.count; ++i) {
            prRawStringReset(&properties.data[i].name);
            prRawStringReset(&properties.data[i].value);
        }
        frames.reset();
        properties.reset();
        keyframes.reset();
        views.reset();
    }
};

// A property of a keyframe object: its IDL and CSS names, and the atom that reads it.
struct AnimationProperty {
    PrRawString idl;
    PrRawString css;
    JSAtom atom;
};

void resetProperties(JSContext* ctx, RawArray<AnimationProperty>& properties) {
    for (uint32_t i = 0; i < properties.count; ++i) {
        prRawStringReset(&properties.data[i].idl);
        prRawStringReset(&properties.data[i].css);
        JS_FreeAtom(ctx, properties.data[i].atom);
    }
    properties.reset();
}

// Writes the animation properties of `object`: its own enumerable string keys that name an animatable property, a
// shorthand of one, or a custom property, in code point order of their IDL names.
bool animationProperties(JSContext* ctx, JSValueConst object, RawArray<AnimationProperty>* out) {
    JSPropertyEnum* names = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
        return false;
    PrTextBuffer css{};
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; ++i) {
        const char* text = JS_AtomToCString(ctx, names[i].atom);
        ok = text != nullptr;
        if (!ok)
            break;
        PrStringView idl(text);
        if (animationPropertyName(idl, &css) && prCssPropertyAnimatable(prTextBufferView(&css)))
            out->push(AnimationProperty{
                prRawStringCopy(idl), prRawStringCopy(prTextBufferView(&css)),
                JS_DupAtom(ctx, names[i].atom),
            });
        JS_FreeCString(ctx, text);
    }
    prTextBufferReset(&css);
    JS_FreePropertyEnum(ctx, names, count);
    sortItems(out->span(), [](const AnimationProperty& x, const AnimationProperty& y) {
        return prStringCompare(prRawStringView(&x.idl), prRawStringView(&y.idl)) < 0;
    });
    return ok;
}

// Reads the composite, easing and offset of a BaseKeyframe, in that order, into `out`.
bool readBaseKeyframe(JSContext* ctx, JSValueConst object, ReadKeyframe* out) {
    JSValue value = JS_GetPropertyStr(ctx, object, "composite");
    if (JS_IsException(value))
        return false;
    bool ok = true;
    if (!JS_IsUndefined(value)) {
        uint32_t composite = 0;
        ok = readEnum(ctx, value, kComposites, countOf(kComposites), &composite);
        out->composite = PrCssComposite(composite);
    }
    JS_FreeValue(ctx, value);
    if (!ok)
        return false;
    value = JS_GetPropertyStr(ctx, object, "easing");
    if (JS_IsException(value))
        return false;
    if (!JS_IsUndefined(value)) {
        PrStringView text{};
        ok = jsReadString(ctx, value, &text);
        if (ok)
            prRawStringReplace(&out->easing, text);
    }
    JS_FreeValue(ctx, value);
    if (!ok)
        return false;
    value = JS_GetPropertyStr(ctx, object, "offset");
    if (JS_IsException(value))
        return false;
    ok = readOffset(ctx, value, &out->offset);
    JS_FreeValue(ctx, value);
    return ok;
}

// Reads `value` as a DOMString, or as a sequence<DOMString> if `lists` allows one, appending to `out`.
bool readPropertyValues(JSContext* ctx, JSValueConst value, bool lists, RawArray<PrRawString>* out) {
    if (lists && JS_IsObject(value)) {
        JSValue method = jsIteratorMethod(ctx, value);
        if (JS_IsException(method))
            return false;
        if (!JS_IsUndefined(method) && !JS_IsNull(method)) {
            bool ok = jsIterate(ctx, value, method, [&](JSValue item) {
                PrStringView text{};
                bool read = jsReadString(ctx, item, &text);
                if (read)
                    out->push(prRawStringCopy(text));
                JS_FreeValue(ctx, item);
                return read;
            });
            JS_FreeValue(ctx, method);
            return ok;
        }
        JS_FreeValue(ctx, method);
    }
    PrStringView text{};
    if (!jsReadString(ctx, value, &text))
        return false;
    out->push(prRawStringCopy(text));
    return true;
}

void resetStrings(RawArray<PrRawString>& strings) {
    for (uint32_t i = 0; i < strings.count; ++i)
        prRawStringReset(&strings.data[i]);
    strings.reset();
}

// Reads a sequence or a single value of a member of BasePropertyIndexedKeyframe: offset, easing or composite. A single
// value reads as one item.
template <typename Read>
bool readIndexedMember(JSContext* ctx, JSValueConst object, const char* name, Read read) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsException(value))
        return false;
    bool ok = true;
    if (!JS_IsUndefined(value)) {
        JSValue method = JS_IsObject(value) ? jsIteratorMethod(ctx, value) : JS_UNDEFINED;
        if (JS_IsException(method)) {
            ok = false;
        } else if (!JS_IsUndefined(method) && !JS_IsNull(method)) {
            ok = jsIterate(ctx, value, method, [&](JSValue item) {
                bool r = read(item);
                JS_FreeValue(ctx, item);
                return r;
            });
        } else {
            ok = read(value);
        }
        JS_FreeValue(ctx, method);
    }
    JS_FreeValue(ctx, value);
    return ok;
}

// Reads the keyframes of the list form: each item is a keyframe.
bool readKeyframeList(JSContext* ctx, JSValueConst input, JSValueConst method, ReadKeyframes* out) {
    return jsIterate(ctx, input, method, [&](JSValue item) {
        bool read = true;
        if (!JS_IsObject(item) && !JS_IsNull(item) && !JS_IsUndefined(item)) {
            JS_ThrowTypeError(ctx, "A keyframe is not an object");
            read = false;
        }
        ReadKeyframe& frame = out->add(kPrF64Nan);
        if (read && JS_IsObject(item)) {
            RawArray<AnimationProperty> properties;
            read = animationProperties(ctx, item, &properties) && readBaseKeyframe(ctx, item, &frame);
            for (uint32_t i = 0; read && i < properties.count; ++i) {
                JSValue value = JS_GetProperty(ctx, item, properties.data[i].atom);
                RawArray<PrRawString> values;
                read = !JS_IsException(value) && readPropertyValues(ctx, value, false, &values);
                if (read) {
                    out->addProperty(prRawStringClone(&properties.data[i].css), values.data[0]);
                    values.data[0] = prRawStringNull();
                }
                resetStrings(values);
                JS_FreeValue(ctx, value);
            }
            resetProperties(ctx, properties);
        }
        JS_FreeValue(ctx, item);
        return read;
    });
}

// Reads the keyframes of the property-indexed form: each property's values are spaced evenly, and the keyframes at
// equal offsets merge. The offsets apply to the keyframes in order, and an easing list or a composite list repeats over
// them.
bool readIndexedKeyframes(JSContext* ctx, JSValueConst input, ReadKeyframes* out) {
    RawArray<double> offsets;
    RawArray<PrRawString> easings;
    RawArray<PrCssComposite> composites;
    bool ok = readIndexedMember(ctx, input, "composite", [&](JSValueConst value) {
        uint32_t composite = 0;
        bool read = readEnum(ctx, value, kComposites, countOf(kComposites), &composite);
        composites.push(PrCssComposite(composite));
        return read;
    });
    ok = ok && readIndexedMember(ctx, input, "easing", [&](JSValueConst value) {
        PrStringView text{};
        bool read = jsReadString(ctx, value, &text);
        easings.push(prRawStringCopy(read ? text : PrStringView{}));
        return read;
    });
    ok = ok && readIndexedMember(ctx, input, "offset", [&](JSValueConst value) {
        double offset = kPrF64Nan;
        bool read = readOffset(ctx, value, &offset);
        offsets.push(offset);
        return read;
    });
    // A value of a property at the offset that its position among the property's values spaces it at.
    struct IndexedValue {
        double offset;
        uint32_t property;
        PrRawString value;
    };
    RawArray<AnimationProperty> properties;
    RawArray<IndexedValue> values;
    ok = ok && animationProperties(ctx, input, &properties);
    for (uint32_t i = 0; ok && i < properties.count; ++i) {
        JSValue value = JS_GetProperty(ctx, input, properties.data[i].atom);
        RawArray<PrRawString> list;
        ok = !JS_IsException(value) && readPropertyValues(ctx, value, true, &list);
        JS_FreeValue(ctx, value);
        for (uint32_t k = 0; ok && k < list.count; ++k) {
            double offset = list.count == 1 ? 1 : double(k) / double(list.count - 1);
            values.push(IndexedValue{offset, i, list.data[k]});
            list.data[k] = prRawStringNull();
        }
        resetStrings(list);
    }
    stableSortItems(values.span(), [](const IndexedValue& x, const IndexedValue& y) { return x.offset < y.offset; });
    for (uint32_t i = 0; ok && i < values.count; ++i) {
        const IndexedValue& value = values.data[i];
        if (out->frames.count == 0 || out->frames.data[out->frames.count - 1].computedOffset != value.offset)
            out->add(value.offset);
        out->addProperty(prRawStringClone(&properties.data[value.property].css), value.value);
        values.data[i].value = prRawStringNull();
    }
    for (uint32_t i = 0; i < values.count; ++i)
        prRawStringReset(&values.data[i].value);
    values.reset();
    resetProperties(ctx, properties);
    for (uint32_t i = 0; ok && i < offsets.count; ++i) {
        double offset = offsets.data[i];
        if (i < out->frames.count) {
            out->frames.data[i].offset = offset;
            if (!prF64IsNan(offset))
                out->frames.data[i].computedOffset = offset;
        } else if (!prF64IsNan(offset) && !(offset >= 0 && offset <= 1)) {
            // An offset past the keyframes is still checked.
            JS_ThrowTypeError(ctx, "An offset is outside [0, 1]");
            ok = false;
        }
    }
    for (uint32_t i = 0; ok && easings.count != 0 && i < out->frames.count; ++i)
        prRawStringReplace(&out->frames.data[i].easing, prRawStringView(&easings.data[i % easings.count]));
    for (uint32_t i = 0; ok && composites.count != 0 && i < out->frames.count; ++i)
        out->frames.data[i].composite = composites.data[i % composites.count];
    // An easing that does not parse throws even where no keyframe would use it.
    for (uint32_t i = 0; ok && i < easings.count; ++i) {
        if (!prCssEasingValid(prRawStringView(&easings.data[i]))) {
            JS_ThrowTypeError(ctx, "An easing is not an <easing-function>");
            ok = false;
        }
    }
    offsets.reset();
    resetStrings(easings);
    composites.reset();
    return ok;
}

// Processes a keyframes argument into `out` (web-animations-1/#processing-a-keyframes-argument).
bool readKeyframes(JSContext* ctx, JSValueConst input, ReadKeyframes* out) {
    if (JS_IsNull(input) || JS_IsUndefined(input))
        return true;
    if (!JS_IsObject(input)) {
        JS_ThrowTypeError(ctx, "The keyframes are not an object");
        return false;
    }
    JSValue method = jsIteratorMethod(ctx, input);
    if (JS_IsException(method))
        return false;
    bool list = !JS_IsUndefined(method) && !JS_IsNull(method);
    bool ok = list ? readKeyframeList(ctx, input, method, out) : readIndexedKeyframes(ctx, input, out);
    JS_FreeValue(ctx, method);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Objects

JSValue timelineToJs(JSContext* ctx, PrCssTimeline* PR_NULLABLE timeline) {
    if (timeline == nullptr)
        return JS_NULL;
    return jsWrap(ctx, documentTimelineClass, JS_UNDEFINED, timeline, false);
}

JSValue effectToJs(JSContext* ctx, PrCssKeyframeEffect* PR_NULLABLE effect) {
    if (effect == nullptr)
        return JS_NULL;
    return jsWrap(ctx, keyframeEffectClass, JS_UNDEFINED, effect, false);
}

JSValue animationToJs(JSContext* ctx, PrCssAnimation* PR_NULLABLE animation) {
    if (animation == nullptr)
        return JS_NULL;
    return prJsDomWrapEventTarget(ctx, JS_UNDEFINED, prCssAnimationAsEventTarget(animation));
}

PrCssAnimation* PR_NULLABLE animationThis(JSContext* ctx, JSValueConst object) {
    PrDomEventTarget* target = jsThis<PrDomEventTarget>(ctx, object, animationClass);
    return target != nullptr ? prCssAnimationFromEventTarget(target) : nullptr;
}

// The slots of an Animation's object: its current ready and finished promises, the generations that they stand for, and
// the functions that settle them.
enum {
    AnimationSlot_Ready,
    AnimationSlot_ReadyResolve,
    AnimationSlot_ReadyReject,
    AnimationSlot_ReadyGeneration,
    AnimationSlot_Finished,
    AnimationSlot_FinishedResolve,
    AnimationSlot_FinishedReject,
    AnimationSlot_FinishedGeneration,
    AnimationSlot_Count,
};
typedef uint8_t AnimationSlot;

uint32_t slotOf(PrCssAnimationPromise promise, uint32_t slot) {
    return promise == PrCssAnimationPromise_Ready ? slot : slot + AnimationSlot_Finished;
}

// Settles the promise in the object's slots with the animation, or rejects it with an AbortError.
void settleSlots(JSContext* ctx, JSValueConst object, PrCssAnimationPromise promise, PrCssPromiseState state) {
    uint32_t base = slotOf(promise, 0);
    JSValue settle = jsReference(ctx, object, base + (state == PrCssPromiseState_Fulfilled ? 1 : 2));
    if (JS_IsUndefined(settle))
        return;
    JSValue value = state == PrCssPromiseState_Fulfilled ? JS_DupValue(ctx, object) : jsDomExceptionNew(ctx, "AbortError");
    JS_FreeValue(ctx, JS_Call(ctx, settle, JS_UNDEFINED, 1, &value));
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, settle);
    jsSetReference(ctx, object, base + 1, JS_UNDEFINED);
    jsSetReference(ctx, object, base + 2, JS_UNDEFINED);
}

// Marks a promise as handled, so that a rejection with an AbortError is not a script's error
// (web-animations-1/#animation-reset-an-animations-pending-tasks).
void markHandled(JSContext* ctx, JSValueConst promise) {
    JSValue ignore = JS_NewCFunction(ctx, [](JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }, "", 1);
    JSAtom name = JS_NewAtom(ctx, "catch");
    JS_FreeValue(ctx, JS_Invoke(ctx, promise, name, 1, &ignore));
    JS_FreeAtom(ctx, name);
    JS_FreeValue(ctx, ignore);
}

// Returns the object's current promise, made for the animation's current generation in its state.
JSValue promiseOf(JSContext* ctx, JSValueConst object, PrCssAnimation* animation, PrCssAnimationPromise which) {
    uint32_t generation = 0;
    PrCssPromiseState state = prCssAnimationPromiseState(animation, which, &generation);
    uint32_t base = slotOf(which, 0);
    JSValue held = jsReference(ctx, object, base);
    JSValue heldGeneration = jsReference(ctx, object, base + 3);
    uint32_t heldAt = 0;
    bool current = !JS_IsUndefined(held) && JS_ToUint32(ctx, &heldAt, heldGeneration) == 0 && heldAt == generation;
    JS_FreeValue(ctx, heldGeneration);
    if (current)
        return held;
    JS_FreeValue(ctx, held);
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(promise))
        return promise;
    jsSetReference(ctx, object, base, promise);
    jsSetReference(ctx, object, base + 3, JS_NewUint32(ctx, generation));
    jsSetReference(ctx, object, base + 1, functions[0]);
    jsSetReference(ctx, object, base + 2, functions[1]);
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    if (state != PrCssPromiseState_Pending) {
        if (state == PrCssPromiseState_Rejected)
            markHandled(ctx, promise);
        settleSlots(ctx, object, which, state);
    }
    return promise;
}

void promiseSettled(void* PR_NULLABLE context, PrCssAnimation* animation, PrCssAnimationPromise which,
        uint32_t generation, PrCssPromiseState state) {
    JSContext* ctx = static_cast<JSContext*>(context);
    JSValue object = jsWrapperOf(ctx, prJsDomEventTargetClass, prCssAnimationAsEventTarget(animation));
    if (JS_IsUndefined(object))
        return;
    uint32_t base = slotOf(which, 0);
    JSValue heldGeneration = jsReference(ctx, object, base + 3);
    uint32_t heldAt = 0;
    if (JS_ToUint32(ctx, &heldAt, heldGeneration) == 0 && heldAt == generation) {
        if (state == PrCssPromiseState_Rejected) {
            JSValue promise = jsReference(ctx, object, base);
            if (!JS_IsUndefined(promise))
                markHandled(ctx, promise);
            JS_FreeValue(ctx, promise);
        }
        settleSlots(ctx, object, which, state);
    }
    JS_FreeValue(ctx, heldGeneration);
    JS_FreeValue(ctx, object);
}

struct FinishTask {
    PrCssAnimation* animation;
};

void scheduleFinish(void* PR_NULLABLE context, PrCssAnimation* animation) {
    JSContext* ctx = static_cast<JSContext*>(context);
    FinishTask* task = static_cast<FinishTask*>(prMemoryAlloc(sizeof(FinishTask)));
    prCssAnimationRetain(animation);
    task->animation = animation;
    jsEnqueueNative(ctx, [](void* data) {
        FinishTask* task = static_cast<FinishTask*>(data);
        prCssAnimationNotifyFinished(task->animation);
        prCssAnimationRelease(task->animation);
        prMemoryFree(task);
    }, task);
}

// The document holds the object of an animation that runs, which keeps its promises and its event handlers alive.
void holdRunning(JSContext* ctx, PrCssAnimation* animation, JSValueConst object, bool hold) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return;
    JSValue owner = prJsDomWrapDocument(ctx, document);
    if (!JS_IsException(owner))
        jsHold(ctx, owner, "[[RunningAnimations]]", object, hold);
    JS_FreeValue(ctx, owner);
    (void)animation;
}

void listed(void* PR_NULLABLE context, PrCssAnimation* animation, bool running) {
    JSContext* ctx = static_cast<JSContext*>(context);
    JSValue object = jsWrapperOf(ctx, prJsDomEventTargetClass, prCssAnimationAsEventTarget(animation));
    if (!JS_IsUndefined(object))
        holdRunning(ctx, animation, object, running);
    JS_FreeValue(ctx, object);
}

// Makes the binding hear about the animations of the document, the realm's.
void observeAnimations(JSContext* ctx, PrDomDocument* document) {
    const PrCssAnimationCallback callback = {ctx, promiseSettled, scheduleFinish, listed};
    prCssDocumentSetAnimationCallback(document, &callback);
}

void animationWrapped(JSContext* ctx, JSValueConst object, void* handle) {
    PrCssAnimation* animation = prCssAnimationFromEventTarget(static_cast<PrDomEventTarget*>(handle));
    if (animation != nullptr && prCssAnimationRunning(animation))
        holdRunning(ctx, animation, object, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// AnimationTimeline and DocumentTimeline

JSValue timelineCurrentTime(JSContext* ctx, JSValueConst object) {
    PrCssTimeline* timeline = jsThis<PrCssTimeline>(ctx, object, timelineClass);
    if (timeline == nullptr)
        return JS_EXCEPTION;
    return nullableNumber(ctx, prCssTimelineCurrentTime(timeline));
}

JSValue documentTimelineConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_ThrowTypeError(ctx, "There is no document");
    double origin = 0;
    bool present = false;
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!jsDictionary(ctx, options, &present))
        return JS_EXCEPTION;
    if (present) {
        JSValue value = JS_GetPropertyStr(ctx, options, "originTime");
        bool ok = !JS_IsException(value) && (JS_IsUndefined(value) || readFiniteNumber(ctx, value, &origin));
        JS_FreeValue(ctx, value);
        if (!ok)
            return JS_EXCEPTION;
    }
    observeAnimations(ctx, document);
    PrCssTimeline* timeline = prCssTimelineCreate(document, origin);
    JSValue object = jsWrap(ctx, documentTimelineClass, JS_UNDEFINED, timeline, true);
    if (JS_IsException(object)) {
        prCssTimelineRelease(timeline);
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

// duration (web-animations-2/#dom-animationtimeline-duration). A document timeline has none.
JSValue timelineDuration(JSContext* ctx, JSValueConst object) {
    return jsThis<PrCssTimeline>(ctx, object, timelineClass) != nullptr ? JS_NULL : JS_EXCEPTION;
}

constexpr JSCFunctionListEntry kTimelineProto[] = {
    PR_JS_TAG("AnimationTimeline"),
    PR_JS_GET("currentTime", timelineCurrentTime),
    PR_JS_GET("duration", timelineDuration),
};

constexpr JSCFunctionListEntry kDocumentTimelineProto[] = {PR_JS_TAG("DocumentTimeline")};

// ---------------------------------------------------------------------------------------------------------------------
// AnimationEffect and KeyframeEffect

JSValue timingObject(JSContext* ctx, const PrCssEffectTiming& timing) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "delay", JS_NewFloat64(ctx, timing.delay));
    JS_SetPropertyStr(ctx, object, "direction", jsNewStringView(ctx, kDirections[timing.direction]));
    JS_SetPropertyStr(ctx, object, "duration",
        prF64IsNan(timing.duration) ? jsNewStringView(ctx, "auto") : JS_NewFloat64(ctx, timing.duration));
    JS_SetPropertyStr(ctx, object, "easing", jsNewStringView(ctx, timing.easing));
    JS_SetPropertyStr(ctx, object, "endDelay", JS_NewFloat64(ctx, timing.endDelay));
    JS_SetPropertyStr(ctx, object, "fill", jsNewStringView(ctx, kFills[timing.fill]));
    JS_SetPropertyStr(ctx, object, "iterationStart", JS_NewFloat64(ctx, timing.iterationStart));
    JS_SetPropertyStr(ctx, object, "iterations", JS_NewFloat64(ctx, timing.iterations));
    return object;
}

JSValue effectGetTiming(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    PrCssKeyframeEffect* effect = jsThis<PrCssKeyframeEffect>(ctx, object, effectClass);
    if (effect == nullptr)
        return JS_EXCEPTION;
    PrCssEffectTiming timing{};
    prCssKeyframeEffectTiming(effect, &timing);
    return timingObject(ctx, timing);
}

JSValue effectGetComputedTiming(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    PrCssKeyframeEffect* effect = jsThis<PrCssKeyframeEffect>(ctx, object, effectClass);
    if (effect == nullptr)
        return JS_EXCEPTION;
    PrCssEffectTiming timing{};
    PrCssComputedTiming computed{};
    prCssKeyframeEffectComputedTiming(effect, &timing, &computed);
    JSValue result = timingObject(ctx, timing);
    JS_SetPropertyStr(ctx, result, "endTime", JS_NewFloat64(ctx, computed.endTime));
    JS_SetPropertyStr(ctx, result, "activeDuration", JS_NewFloat64(ctx, computed.activeDuration));
    JS_SetPropertyStr(ctx, result, "localTime", nullableNumber(ctx, computed.localTime));
    JS_SetPropertyStr(ctx, result, "progress", nullableNumber(ctx, computed.progress));
    JS_SetPropertyStr(ctx, result, "currentIteration", nullableNumber(ctx, computed.currentIteration));
    return result;
}

JSValue effectUpdateTiming(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrCssKeyframeEffect* effect = jsThis<PrCssKeyframeEffect>(ctx, object, effectClass);
    if (effect == nullptr)
        return JS_EXCEPTION;
    PrCssEffectTiming timing{};
    prCssKeyframeEffectTiming(effect, &timing);
    PrCssTimingMembers members = 0;
    StringArgument easing = {ctx};
    if (!readTiming(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &timing, &members, &easing))
        return JS_EXCEPTION;
    PrStatus status = prCssKeyframeEffectUpdateTiming(effect, &timing, members);
    return status == PrStatus_Ok ? JS_UNDEFINED : throwStatus(ctx, status);
}

// Reads the options of a KeyframeEffect or of animate(): a duration, or a dictionary of timing and of the effect's
// composite, iterationComposite and pseudoElement. `id` is read only for animate().
struct EffectOptions {
    PrCssEffectTiming timing = prCssEffectTimingDefault();
    PrCssComposite composite = PrCssComposite_Replace;
    PrCssComposite iterationComposite = PrCssComposite_Replace;
    PrStringView pseudo{};
    StringArgument pseudoText;
    StringArgument easing;
    StringArgument id;
    bool hasId = false;
    JSValue timeline = JS_UNDEFINED;          // animate()'s timeline member, as given
    bool hasTimeline = false;

    explicit EffectOptions(JSContext* ctx) : pseudoText{ctx}, easing{ctx}, id{ctx} {}
};

// Returns true if `text` is a value of `property`, animation-range-start or animation-range-end, whose lengths depend
// on no font (scroll-animations-1/#named-range-animation-declaration): a range does not change with the target's font.
bool validRange(PrCssProperty property, PrStringView text) {
    PrCssDeclarationName name{.property = property, .descriptor = PrCssDescriptor_None};
    PrCssValue* value = prCssValueCreate();
    bool valid = prCssValueParse(&name, text, value) == PrStatus_Ok;
    for (uint32_t node = 0; valid && node < prCssValueSize(value); ++node) {
        PrCssValueFields fields = prCssValueFields(value, node);
        valid = fields.kind != PrCssValueKind_Dimension || fields.number.unit < PrCssUnit_Em ||
            fields.number.unit > PrCssUnit_Rlh;
    }
    prCssValueDestroy(value);
    return valid;
}

// Checks animate()'s range boundary `member`, a value of `property`, and throws a TypeError if it is not one
// (web-animations-2/#dom-keyframeanimationoptions-rangestart). The boundary is a string, a typed value, which reads as
// its CSS text, or a TimelineRangeOffset, which reads as its range name followed by its offset. The engine has no
// timelines that ranges apply to, so a valid range has no effect.
bool checkRange(JSContext* ctx, JSValueConst options, const char* member, PrCssProperty property) {
    JSValue value = JS_GetPropertyStr(ctx, options, member);
    if (JS_IsException(value))
        return false;
    bool ok = true;
    PrTextBuffer text = {};
    if (!JS_IsUndefined(value)) {
        JSValue rangeName = JS_IsObject(value) ? JS_GetPropertyStr(ctx, value, "rangeName") : JS_UNDEFINED;
        JSValue offset = JS_IsObject(value) ? JS_GetPropertyStr(ctx, value, "offset") : JS_UNDEFINED;
        ok = !JS_IsException(rangeName) && !JS_IsException(offset);
        bool dictionary = !JS_IsUndefined(rangeName) || !JS_IsUndefined(offset);
        const JSValueConst parts[] = {dictionary ? rangeName : JS_UNDEFINED, dictionary ? offset : value};
        for (JSValueConst part : parts) {
            StringArgument read = {ctx};
            if (!ok || JS_IsUndefined(part))
                continue;
            ok = read.read(part);
            if (prTextBufferView(&text).size != 0)
                prTextBufferAppendChar(&text, ' ');
            prTextBufferAppend(&text, read.value());
        }
        JS_FreeValue(ctx, rangeName);
        JS_FreeValue(ctx, offset);
        if (ok && !validRange(property, prTextBufferView(&text))) {
            JS_ThrowTypeError(ctx, "Invalid %s", member);
            ok = false;
        }
    }
    prTextBufferReset(&text);
    JS_FreeValue(ctx, value);
    return ok;
}

bool readEffectOptions(JSContext* ctx, JSValueConst options, bool animate, EffectOptions* out) {
    if (JS_IsNumber(options)) {
        double duration = 0;
        if (JS_ToFloat64(ctx, &duration, options) != 0)
            return false;
        if (prF64IsNan(duration)) {
            JS_ThrowTypeError(ctx, "The duration is NaN");
            return false;
        }
        out->timing.duration = duration;
        return true;
    }
    bool present = false;
    if (!jsDictionary(ctx, options, &present))
        return false;
    if (!present)
        return true;
    // KeyframeEffectOptions inherits EffectTiming, whose members come first. Then composite, iterationComposite and
    // pseudoElement, and for animate() id and timeline.
    PrCssTimingMembers members = 0;
    if (!readTiming(ctx, options, &out->timing, &members, &out->easing))
        return false;
    JSValue value = JS_GetPropertyStr(ctx, options, "composite");
    bool ok = !JS_IsException(value);
    if (ok && !JS_IsUndefined(value)) {
        uint32_t composite = 0;
        ok = readEnum(ctx, value, kComposites, 3, &composite);
        out->composite = PrCssComposite(composite);
    }
    JS_FreeValue(ctx, value);
    if (!ok)
        return false;
    value = JS_GetPropertyStr(ctx, options, "iterationComposite");
    ok = !JS_IsException(value);
    if (ok && !JS_IsUndefined(value)) {
        uint32_t composite = 0;
        ok = readEnum(ctx, value, kComposites, 3, &composite);
        out->iterationComposite = PrCssComposite(composite);
    }
    JS_FreeValue(ctx, value);
    if (!ok)
        return false;
    value = JS_GetPropertyStr(ctx, options, "pseudoElement");
    ok = !JS_IsException(value);
    if (ok && !JS_IsUndefined(value) && !JS_IsNull(value)) {
        ok = out->pseudoText.read(value);
        out->pseudo = out->pseudoText.value();
    }
    JS_FreeValue(ctx, value);
    if (!ok || !animate)
        return ok;
    bool given = false;
    if (!jsDictString(ctx, options, "id", &out->id, &given))
        return false;
    out->hasId = given;
    value = JS_GetPropertyStr(ctx, options, "timeline");
    if (JS_IsException(value))
        return false;
    out->hasTimeline = !JS_IsUndefined(value);
    out->timeline = value;
    return checkRange(ctx, options, "rangeStart", PrCssProperty_AnimationRangeStart) &&
        checkRange(ctx, options, "rangeEnd", PrCssProperty_AnimationRangeEnd);
}

// Makes a keyframe effect of `target` from a keyframes argument and options, or throws.
PrCssKeyframeEffect* PR_NULLABLE makeEffect(JSContext* ctx, PrDomDocument* document, PrDomElement* PR_NULLABLE target,
        JSValueConst keyframes, EffectOptions& options) {
    ReadKeyframes read;
    PrCssKeyframeEffect* effect = nullptr;
    PrStatus status = PrStatus_InvalidData;
    if (readKeyframes(ctx, keyframes, &read)) {
        status = prCssKeyframeEffectCreate(document, target, options.pseudo, read.build(), read.frames.count,
            &options.timing, &effect);
        if (status != PrStatus_Ok)
            throwStatus(ctx, status);
    }
    read.reset();
    if (status != PrStatus_Ok)
        return nullptr;
    prCssKeyframeEffectSetComposite(effect, options.composite);
    prCssKeyframeEffectSetIterationComposite(effect, options.iterationComposite);
    return effect;
}

// Reads an (Element or CSSPseudoElement)? target. A null target is no element.
bool readTarget(JSContext* ctx, JSValueConst value, PrDomElement* PR_NULLABLE* out) {
    *out = nullptr;
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return true;
    PrDomElement* element = jsHandleAs<PrDomElement>(ctx, value, prJsDomElementClass);
    if (element == nullptr) {
        JS_ThrowTypeError(ctx, "The target is not an Element");
        return false;
    }
    *out = element;
    return true;
}

JSValue keyframeEffectConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_ThrowTypeError(ctx, "There is no document");
    if (!jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrCssKeyframeEffect* effect = nullptr;
    // new KeyframeEffect(source) copies the source (web-animations-1/#dom-keyframeeffect-keyframeeffect-source).
    if (argc == 1 && JS_IsObject(argv[0]) && jsHandleAs<PrCssKeyframeEffect>(ctx, argv[0], keyframeEffectClass)) {
        effect = prCssKeyframeEffectClone(jsHandleAs<PrCssKeyframeEffect>(ctx, argv[0], keyframeEffectClass));
    } else {
        PrDomElement* target = nullptr;
        if (!readTarget(ctx, argv[0], &target))
            return JS_EXCEPTION;
        EffectOptions options(ctx);
        // The options are read before the keyframes (web-animations-1/#dom-keyframeeffect-keyframeeffect).
        if (!readEffectOptions(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, false, &options))
            return JS_EXCEPTION;
        effect = makeEffect(ctx, document, target, argc > 1 ? argv[1] : JS_NULL, options);
        if (effect == nullptr)
            return JS_EXCEPTION;
    }
    observeAnimations(ctx, document);
    JSValue object = jsWrap(ctx, keyframeEffectClass, JS_UNDEFINED, effect, true);
    if (JS_IsException(object)) {
        prCssKeyframeEffectRelease(effect);
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

PrCssKeyframeEffect* PR_NULLABLE keyframeEffectThis(JSContext* ctx, JSValueConst object) {
    return jsThis<PrCssKeyframeEffect>(ctx, object, keyframeEffectClass);
}

JSValue effectTarget(JSContext* ctx, JSValueConst object) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    if (effect == nullptr)
        return JS_EXCEPTION;
    PrDomElement* target = prCssKeyframeEffectTarget(effect);
    return target != nullptr ? prJsDomWrapEventTarget(ctx, JS_UNDEFINED, prDomElementAsEventTarget(target)) : JS_NULL;
}

JSValue setEffectTarget(JSContext* ctx, JSValueConst object, JSValueConst value) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    PrDomElement* target = nullptr;
    if (effect == nullptr || !readTarget(ctx, value, &target))
        return JS_EXCEPTION;
    prCssKeyframeEffectSetTarget(effect, target);
    return JS_UNDEFINED;
}

JSValue effectPseudoElement(JSContext* ctx, JSValueConst object) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    if (effect == nullptr)
        return JS_EXCEPTION;
    PrStringView pseudo = prCssKeyframeEffectPseudoElement(effect);
    return pseudo.data != nullptr ? jsNewStringView(ctx, pseudo) : JS_NULL;
}

JSValue setEffectPseudoElement(JSContext* ctx, JSValueConst object, JSValueConst value) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    if (effect == nullptr)
        return JS_EXCEPTION;
    StringArgument pseudo = {ctx};
    if (!JS_IsNull(value) && !pseudo.read(value))
        return JS_EXCEPTION;
    PrStatus status = prCssKeyframeEffectSetPseudoElement(effect, JS_IsNull(value) ? PrStringView{} : pseudo.value());
    return status == PrStatus_Ok ? JS_UNDEFINED : throwStatus(ctx, status);
}

// Implements composite (magic 0) and iterationComposite (magic 1).
JSValue effectComposite(JSContext* ctx, JSValueConst object, int magic) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    if (effect == nullptr)
        return JS_EXCEPTION;
    PrCssComposite composite =
        magic == 0 ? prCssKeyframeEffectComposite(effect) : prCssKeyframeEffectIterationComposite(effect);
    return jsNewStringView(ctx, kComposites[composite]);
}

JSValue setEffectComposite(JSContext* ctx, JSValueConst object, JSValueConst value, int magic) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    if (effect == nullptr)
        return JS_EXCEPTION;
    PrStringView text{};
    if (!jsReadString(ctx, value, &text))
        return JS_EXCEPTION;
    // An assignment of a value outside the enumeration is ignored (webidl/#js-enumeration).
    for (uint32_t i = 0; i < 3; ++i) {
        if (text != kComposites[i])
            continue;
        if (magic == 0)
            prCssKeyframeEffectSetComposite(effect, PrCssComposite(i));
        else
            prCssKeyframeEffectSetIterationComposite(effect, PrCssComposite(i));
    }
    return JS_UNDEFINED;
}

JSValue effectGetKeyframes(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    if (effect == nullptr)
        return JS_EXCEPTION;
    uint32_t count = prCssKeyframeEffectKeyframes(effect, nullptr, 0);
    RawArray<PrCssComputedKeyframe> frames;
    frames.resize(count);
    prCssKeyframeEffectKeyframes(effect, frames.data, count);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        const PrCssComputedKeyframe& frame = frames.data[i];
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "offset", nullableNumber(ctx, frame.offset));
        JS_SetPropertyStr(ctx, item, "computedOffset", JS_NewFloat64(ctx, frame.computedOffset));
        JS_SetPropertyStr(ctx, item, "easing", jsNewString(ctx, &frame.easing));
        JS_SetPropertyStr(ctx, item, "composite", jsNewStringView(ctx, kComposites[frame.composite]));
        for (uint32_t k = 0; k < frame.propertyCount; ++k) {
            PrRawString name = prRawStringNull();
            PrRawString value = prRawStringNull();
            if (prCssKeyframeEffectKeyframeProperty(effect, i, k, &name, &value)) {
                PrRawString idl = idlPropertyName(prRawStringView(&name));
                PrStringView view = prRawStringView(&idl);
                JSAtom atom = JS_NewAtomLen(ctx, view.data, view.size);
                JS_SetProperty(ctx, item, atom, jsNewString(ctx, &value));
                JS_FreeAtom(ctx, atom);
                prRawStringReset(&idl);
            }
            prRawStringReset(&name);
            prRawStringReset(&value);
        }
        JS_SetPropertyUint32(ctx, array, i, item);
        prRawStringReset(&frames.data[i].easing);
    }
    frames.reset();
    return array;
}

JSValue effectSetKeyframes(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrCssKeyframeEffect* effect = keyframeEffectThis(ctx, object);
    if (effect == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    ReadKeyframes read;
    bool ok = readKeyframes(ctx, argv[0], &read);
    PrStatus status = ok ? prCssKeyframeEffectSetKeyframes(effect, read.build(), read.frames.count) : PrStatus_Ok;
    read.reset();
    if (!ok)
        return JS_EXCEPTION;
    return status == PrStatus_Ok ? JS_UNDEFINED : throwStatus(ctx, status);
}

constexpr JSCFunctionListEntry kEffectProto[] = {
    PR_JS_TAG("AnimationEffect"),
    PR_JS_METHOD("getTiming", 0, effectGetTiming),
    PR_JS_METHOD("getComputedTiming", 0, effectGetComputedTiming),
    PR_JS_METHOD("updateTiming", 0, effectUpdateTiming),
};

constexpr JSCFunctionListEntry kKeyframeEffectProto[] = {
    PR_JS_TAG("KeyframeEffect"),
    PR_JS_GET_SET("target", effectTarget, setEffectTarget),
    PR_JS_GET_SET("pseudoElement", effectPseudoElement, setEffectPseudoElement),
    PR_JS_MAGIC_GET_SET("iterationComposite", effectComposite, setEffectComposite, 1),
    PR_JS_MAGIC_GET_SET("composite", effectComposite, setEffectComposite, 0),
    PR_JS_METHOD("getKeyframes", 0, effectGetKeyframes),
    PR_JS_METHOD("setKeyframes", 1, effectSetKeyframes),
};

// ---------------------------------------------------------------------------------------------------------------------
// Animation

// Reads an AnimationTimeline?, which a timeline of the realm's document or null gives.
bool readTimeline(JSContext* ctx, JSValueConst value, PrCssTimeline* PR_NULLABLE* out) {
    *out = nullptr;
    if (JS_IsNull(value))
        return true;
    PrCssTimeline* timeline = jsHandleAs<PrCssTimeline>(ctx, value, timelineClass);
    if (timeline == nullptr) {
        JS_ThrowTypeError(ctx, "The value is not an AnimationTimeline");
        return false;
    }
    *out = timeline;
    return true;
}

JSValue animationConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_ThrowTypeError(ctx, "There is no document");
    PrCssKeyframeEffect* effect = nullptr;
    if (jsArgGiven(argc, argv, 0) && !JS_IsNull(argv[0])) {
        effect = jsHandleAs<PrCssKeyframeEffect>(ctx, argv[0], effectClass);
        if (effect == nullptr)
            return JS_ThrowTypeError(ctx, "The effect is not an AnimationEffect");
    }
    PrCssTimeline* timeline = prCssDocumentTimeline(document);
    if (jsArgGiven(argc, argv, 1) && !readTimeline(ctx, argv[1], &timeline))
        return JS_EXCEPTION;
    observeAnimations(ctx, document);
    PrCssAnimation* animation = prCssAnimationCreate(document, effect, timeline);
    JSValue object = animationToJs(ctx, animation);
    prCssAnimationRelease(animation);
    if (JS_IsException(object))
        return object;
    return jsConstructed(ctx, newTarget, object);
}

JSValue animationId(JSContext* ctx, JSValueConst object) {
    PrCssAnimation* animation = animationThis(ctx, object);
    return animation != nullptr ? jsNewStringView(ctx, prCssAnimationId(animation)) : JS_EXCEPTION;
}

JSValue setAnimationId(JSContext* ctx, JSValueConst object, JSValueConst value) {
    PrCssAnimation* animation = animationThis(ctx, object);
    PrStringView text{};
    if (animation == nullptr || !jsReadString(ctx, value, &text))
        return JS_EXCEPTION;
    prCssAnimationSetId(animation, text);
    return JS_UNDEFINED;
}

JSValue animationEffect(JSContext* ctx, JSValueConst object) {
    PrCssAnimation* animation = animationThis(ctx, object);
    return animation != nullptr ? effectToJs(ctx, prCssAnimationEffect(animation)) : JS_EXCEPTION;
}

JSValue setAnimationEffect(JSContext* ctx, JSValueConst object, JSValueConst value) {
    PrCssAnimation* animation = animationThis(ctx, object);
    if (animation == nullptr)
        return JS_EXCEPTION;
    PrCssKeyframeEffect* effect = nullptr;
    if (!JS_IsNull(value)) {
        effect = jsHandleAs<PrCssKeyframeEffect>(ctx, value, effectClass);
        if (effect == nullptr)
            return JS_ThrowTypeError(ctx, "The value is not an AnimationEffect");
    }
    prCssAnimationSetEffect(animation, effect);
    return JS_UNDEFINED;
}

JSValue animationTimeline(JSContext* ctx, JSValueConst object) {
    PrCssAnimation* animation = animationThis(ctx, object);
    return animation != nullptr ? timelineToJs(ctx, prCssAnimationTimeline(animation)) : JS_EXCEPTION;
}

JSValue setAnimationTimeline(JSContext* ctx, JSValueConst object, JSValueConst value) {
    PrCssAnimation* animation = animationThis(ctx, object);
    PrCssTimeline* timeline = nullptr;
    if (animation == nullptr || !readTimeline(ctx, value, &timeline))
        return JS_EXCEPTION;
    prCssAnimationSetTimeline(animation, timeline);
    return JS_UNDEFINED;
}

// Implements startTime (magic 0), currentTime (1) and playbackRate (2).
JSValue animationTime(JSContext* ctx, JSValueConst object, int magic) {
    PrCssAnimation* animation = animationThis(ctx, object);
    if (animation == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        return nullableNumber(ctx, prCssAnimationStartTime(animation));
    if (magic == 1)
        return nullableNumber(ctx, prCssAnimationCurrentTime(animation));
    return JS_NewFloat64(ctx, prCssAnimationPlaybackRate(animation));
}

JSValue setAnimationTime(JSContext* ctx, JSValueConst object, JSValueConst value, int magic) {
    PrCssAnimation* animation = animationThis(ctx, object);
    if (animation == nullptr)
        return JS_EXCEPTION;
    double time = 0;
    if (magic == 2) {
        if (!readFiniteNumber(ctx, value, &time))
            return JS_EXCEPTION;
        prCssAnimationSetPlaybackRate(animation, time);
        return JS_UNDEFINED;
    }
    // CSSNumberish?: a number in milliseconds, or null.
    if (!readNullableNumber(ctx, value, &time))
        return JS_EXCEPTION;
    PrStatus status = magic == 0 ? prCssAnimationSetStartTime(animation, time) : prCssAnimationSetCurrentTime(animation, time);
    if (status != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "The current time cannot be made unresolved");
    return JS_UNDEFINED;
}

// Implements playState (magic 0), replaceState (1) and pending (2).
JSValue animationState(JSContext* ctx, JSValueConst object, int magic) {
    PrCssAnimation* animation = animationThis(ctx, object);
    if (animation == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        return jsNewStringView(ctx, kPlayStates[prCssAnimationPlayState(animation)]);
    if (magic == 1)
        return jsNewStringView(ctx, kReplaceStates[prCssAnimationReplaceState(animation)]);
    return JS_NewBool(ctx, prCssAnimationPending(animation));
}

// Implements ready (magic 0) and finished (1).
JSValue animationPromise(JSContext* ctx, JSValueConst object, int magic) {
    PrCssAnimation* animation = animationThis(ctx, object);
    if (animation == nullptr)
        return JS_EXCEPTION;
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document != nullptr)
        observeAnimations(ctx, document);
    return promiseOf(ctx, object, animation, magic == 0 ? PrCssAnimationPromise_Ready : PrCssAnimationPromise_Finished);
}

// Implements cancel, finish, play, pause, reverse and persist, selected by the magic.
enum {
    Control_Cancel,
    Control_Finish,
    Control_Play,
    Control_Pause,
    Control_Reverse,
    Control_Persist,
    Control_CommitStyles,
};
typedef uint8_t Control;

JSValue animationControl(JSContext* ctx, JSValueConst object, int, JSValueConst*, int magic) {
    PrCssAnimation* animation = animationThis(ctx, object);
    if (animation == nullptr)
        return JS_EXCEPTION;
    PrStatus status = PrStatus_Ok;
    switch (magic) {
        case Control_Cancel: prCssAnimationCancel(animation); break;
        case Control_Finish: status = prCssAnimationFinish(animation); break;
        case Control_Play: status = prCssAnimationPlay(animation); break;
        case Control_Pause: status = prCssAnimationPause(animation); break;
        case Control_Reverse: status = prCssAnimationReverse(animation); break;
        case Control_Persist: prCssAnimationPersist(animation); break;
        case Control_CommitStyles: status = prCssAnimationCommitStyles(animation); break;
        default: break;
    }
    if (status == PrStatus_Ok)
        return JS_UNDEFINED;
    if (magic == Control_CommitStyles || magic != Control_Finish || status == PrStatus_InvalidState)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return throwStatus(ctx, status);
}

JSValue animationUpdatePlaybackRate(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrCssAnimation* animation = animationThis(ctx, object);
    if (animation == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    double rate = 0;
    if (!readFiniteNumber(ctx, argv[0], &rate))
        return JS_EXCEPTION;
    prCssAnimationUpdatePlaybackRate(animation, rate);
    return JS_UNDEFINED;
}

// Implements animationName of a CSSAnimation and transitionProperty of a CSSTransition.
JSValue animationName(JSContext* ctx, JSValueConst object) {
    PrCssAnimation* animation = animationThis(ctx, object);
    return animation != nullptr ? jsNewStringView(ctx, prCssAnimationName(animation)) : JS_EXCEPTION;
}

constexpr JSCFunctionListEntry kAnimationProto[] = {
    PR_JS_TAG("Animation"),
    PR_JS_GET_SET("id", animationId, setAnimationId),
    PR_JS_GET_SET("effect", animationEffect, setAnimationEffect),
    PR_JS_GET_SET("timeline", animationTimeline, setAnimationTimeline),
    PR_JS_MAGIC_GET_SET("startTime", animationTime, setAnimationTime, 0),
    PR_JS_MAGIC_GET_SET("currentTime", animationTime, setAnimationTime, 1),
    PR_JS_MAGIC_GET_SET("playbackRate", animationTime, setAnimationTime, 2),
    PR_JS_MAGIC_GET("playState", animationState, 0),
    PR_JS_MAGIC_GET("replaceState", animationState, 1),
    PR_JS_MAGIC_GET("pending", animationState, 2),
    PR_JS_MAGIC_GET("ready", animationPromise, 0),
    PR_JS_MAGIC_GET("finished", animationPromise, 1),
    PR_JS_DOM_EVENT_HANDLER(onfinish),
    PR_JS_DOM_EVENT_HANDLER(oncancel),
    PR_JS_DOM_EVENT_HANDLER(onremove),
    PR_JS_MAGIC("cancel", 0, animationControl, Control_Cancel),
    PR_JS_MAGIC("finish", 0, animationControl, Control_Finish),
    PR_JS_MAGIC("play", 0, animationControl, Control_Play),
    PR_JS_MAGIC("pause", 0, animationControl, Control_Pause),
    PR_JS_METHOD("updatePlaybackRate", 1, animationUpdatePlaybackRate),
    PR_JS_MAGIC("reverse", 0, animationControl, Control_Reverse),
    PR_JS_MAGIC("persist", 0, animationControl, Control_Persist),
    PR_JS_MAGIC("commitStyles", 0, animationControl, Control_CommitStyles),
};

constexpr JSCFunctionListEntry kCssAnimationProto[] = {
    PR_JS_TAG("CSSAnimation"),
    PR_JS_GET("animationName", animationName),
};

constexpr JSCFunctionListEntry kCssTransitionProto[] = {
    PR_JS_TAG("CSSTransition"),
    PR_JS_GET("transitionProperty", animationName),
};

// ---------------------------------------------------------------------------------------------------------------------
// AnimationPlaybackEvent

JSValue playbackEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    PrCssAnimationPlaybackEventInit init = prCssAnimationPlaybackEventInitDefault();
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, dictionary, &present))
        return JS_EXCEPTION;
    if (present) {
        bool ok = jsDictBool(ctx, dictionary, "bubbles", &init.event.bubbles) &&
            jsDictBool(ctx, dictionary, "cancelable", &init.event.cancelable) &&
            jsDictBool(ctx, dictionary, "composed", &init.event.composed);
        JSValue value = ok ? JS_GetPropertyStr(ctx, dictionary, "currentTime") : JS_EXCEPTION;
        ok = ok && !JS_IsException(value) && readNullableNumber(ctx, value, &init.currentTime);
        JS_FreeValue(ctx, value);
        value = ok ? JS_GetPropertyStr(ctx, dictionary, "timelineTime") : JS_EXCEPTION;
        ok = ok && !JS_IsException(value) && readNullableNumber(ctx, value, &init.timelineTime);
        JS_FreeValue(ctx, value);
        if (!ok)
            return JS_EXCEPTION;
    }
    PrCssAnimationPlaybackEvent* event = prCssAnimationPlaybackEventCreate(type.value(), &init);
    JSValue object = jsWrap(ctx, playbackEventClass, JS_UNDEFINED, prCssAnimationPlaybackEventAsEvent(event), true);
    if (JS_IsException(object)) {
        prDomEventRelease(prCssAnimationPlaybackEventAsEvent(event));
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

// Implements currentTime (magic 0) and timelineTime (1).
JSValue playbackEventTime(JSContext* ctx, JSValueConst object, int magic) {
    PrDomEvent* base = jsThis<PrDomEvent>(ctx, object, playbackEventClass);
    PrCssAnimationPlaybackEvent* event = base != nullptr ? prCssAnimationPlaybackEventFromEvent(base) : nullptr;
    if (event == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        return nullableNumber(ctx, prCssAnimationPlaybackEventCurrentTime(event));
    return nullableNumber(ctx, prCssAnimationPlaybackEventTimelineTime(event));
}

constexpr JSCFunctionListEntry kPlaybackEventProto[] = {
    PR_JS_TAG("AnimationPlaybackEvent"),
    PR_JS_MAGIC_GET("currentTime", playbackEventTime, 0),
    PR_JS_MAGIC_GET("timelineTime", playbackEventTime, 1),
};

// ---------------------------------------------------------------------------------------------------------------------
// Animatable and DocumentOrShadowRoot (web-animations-1/#the-animatable-interface-mixin)

JSValue elementAnimate(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, object, prJsDomElementClass);
    if (element == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomDocument* document = prDomNodeOwnerDocument(prDomElementAsNode(element));
    EffectOptions options(ctx);
    if (!readEffectOptions(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, true, &options))
        return JS_EXCEPTION;
    PrCssKeyframeEffect* effect = makeEffect(ctx, document, element, argv[0], options);
    if (effect == nullptr) {
        JS_FreeValue(ctx, options.timeline);
        return JS_EXCEPTION;
    }
    PrCssTimeline* timeline = prCssDocumentTimeline(document);
    if (options.hasTimeline && !readTimeline(ctx, options.timeline, &timeline)) {
        JS_FreeValue(ctx, options.timeline);
        prCssKeyframeEffectRelease(effect);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, options.timeline);
    observeAnimations(ctx, document);
    PrCssAnimation* animation = prCssAnimationCreate(document, effect, timeline);
    prCssKeyframeEffectRelease(effect);
    if (options.hasId)
        prCssAnimationSetId(animation, options.id.value());
    PrStatus status = prCssAnimationPlay(animation);
    JSValue result = status == PrStatus_Ok ? animationToJs(ctx, animation) : throwStatus(ctx, status);
    prCssAnimationRelease(animation);
    return result;
}

// Returns the animations of `root`, or of the document, as an array.
JSValue animationsArray(JSContext* ctx, PrDomDocument* document, PrDomElement* PR_NULLABLE root, bool subtree) {
    observeAnimations(ctx, document);
    uint32_t count = prCssDocumentAnimations(document, root, subtree, nullptr, 0);
    RawArray<PrCssAnimation*> animations;
    animations.resize(count);
    count = prCssDocumentAnimations(document, root, subtree, animations.data, count);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        JS_SetPropertyUint32(ctx, array, i, animationToJs(ctx, animations.data[i]));
        prCssAnimationRelease(animations.data[i]);
    }
    animations.reset();
    return array;
}

JSValue elementGetAnimations(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, object, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    bool subtree = false;
    bool present = false;
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!jsDictionary(ctx, options, &present) || (present && !jsDictBool(ctx, options, "subtree", &subtree)))
        return JS_EXCEPTION;
    return animationsArray(ctx, prDomNodeOwnerDocument(prDomElementAsNode(element)), element, subtree);
}

JSValue documentGetAnimations(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, object, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    return animationsArray(ctx, document, nullptr, true);
}

JSValue shadowRootGetAnimations(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    PrDomShadowRoot* shadow = jsThis<PrDomShadowRoot>(ctx, object, prJsDomShadowRootClass);
    if (shadow == nullptr)
        return JS_EXCEPTION;
    PrDomElement* host = prDomShadowRootHost(shadow);
    PrDomDocument* document = prDomNodeOwnerDocument(prDomElementAsNode(host));
    // The animations of the shadow root's descendants, which are those of its host's subtree less the host's own and
    // those of the host's light tree.
    uint32_t count = prCssDocumentAnimations(document, host, true, nullptr, 0);
    RawArray<PrCssAnimation*> animations;
    animations.resize(count);
    count = prCssDocumentAnimations(document, host, true, animations.data, count);
    JSValue array = JS_NewArray(ctx);
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        PrCssKeyframeEffect* effect = prCssAnimationEffect(animations.data[i]);
        PrDomElement* target = effect != nullptr ? prCssKeyframeEffectTarget(effect) : nullptr;
        if (target != nullptr &&
                prDomNodeGetRootNode(prDomElementAsNode(target), false) == prDomShadowRootAsNode(shadow))
            JS_SetPropertyUint32(ctx, array, at++, animationToJs(ctx, animations.data[i]));
        prCssAnimationRelease(animations.data[i]);
    }
    animations.reset();
    return array;
}

JSValue documentTimeline(JSContext* ctx, JSValueConst object) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, object, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    observeAnimations(ctx, document);
    return jsSameObject(ctx, object, "timeline", [&] { return timelineToJs(ctx, prCssDocumentTimeline(document)); });
}

constexpr JSCFunctionListEntry kElementMembers[] = {
    PR_JS_METHOD("animate", 1, elementAnimate),
    PR_JS_METHOD("getAnimations", 0, elementGetAnimations),
};

constexpr JSCFunctionListEntry kDocumentMembers[] = {
    PR_JS_GET("timeline", documentTimeline),
    PR_JS_METHOD("getAnimations", 0, documentGetAnimations),
};

constexpr JSCFunctionListEntry kShadowRootMembers[] = {PR_JS_METHOD("getAnimations", 0, shadowRootGetAnimations)};

constexpr JsClassDefinition kClasses[] = {
    {
        PR_JS_CLASS("AnimationTimeline", timelineClass, (jsFinalizer<&timelineClass, prCssTimelineRelease>),
            kTimelineProto),
        .references = &kTimelineReferences,
    },
    {
        PR_JS_CLASS("DocumentTimeline", documentTimelineClass,
            (jsFinalizer<&documentTimelineClass, prCssTimelineRelease>), kDocumentTimelineProto),
        .constructor = documentTimelineConstruct,
        .base = &timelineClass,
        .references = &kTimelineReferences,
    },
    {
        PR_JS_CLASS("AnimationEffect", effectClass, (jsFinalizer<&effectClass, prCssKeyframeEffectRelease>),
            kEffectProto),
        .references = &kEffectReferences,
    },
    {
        PR_JS_CLASS("KeyframeEffect", keyframeEffectClass,
            (jsFinalizer<&keyframeEffectClass, prCssKeyframeEffectRelease>), kKeyframeEffectProto),
        .constructor = keyframeEffectConstruct,
        .length = 1,
        .base = &effectClass,
        .references = &kEffectReferences,
    },
    {
        PR_JS_CLASS("Animation", animationClass, jsViewFinalizer<&animationClass>, kAnimationProto),
        .constructor = animationConstruct,
        .base = &prJsDomEventTargetClass,
        .references = &kAnimationReferences,
        .wrapped = animationWrapped,
    },
    {
        PR_JS_CLASS("CSSAnimation", cssAnimationClass, jsViewFinalizer<&cssAnimationClass>, kCssAnimationProto),
        .base = &animationClass,
        .wrapped = animationWrapped,
    },
    {
        PR_JS_CLASS("CSSTransition", cssTransitionClass, jsViewFinalizer<&cssTransitionClass>, kCssTransitionProto),
        .base = &animationClass,
        .wrapped = animationWrapped,
    },
    {
        PR_JS_CLASS("AnimationPlaybackEvent", playbackEventClass, jsViewFinalizer<&playbackEventClass>,
            kPlaybackEventProto),
        .constructor = playbackEventConstruct,
        .length = 1,
        .base = &prJsDomEventClass,
    },
};

void addMembers(JSContext* ctx, JSClassID id, const JSCFunctionListEntry* members, int count) {
    JSValue proto = JS_GetClassProto(ctx, id);
    JS_SetPropertyFunctionList(ctx, proto, members, count);
    JS_FreeValue(ctx, proto);
}

} // namespace

void cssAnimationsBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    addMembers(ctx, prJsDomElementClass, kElementMembers, int(countOf(kElementMembers)));
    addMembers(ctx, prJsDomDocumentClass, kDocumentMembers, int(countOf(kDocumentMembers)));
    // DocumentOrShadowRoot's getAnimations.
    addMembers(ctx, prJsDomShadowRootClass, kShadowRootMembers, int(countOf(kShadowRootMembers)));
}

JSClassID cssAnimationsTargetClass(void* target) {
    PrCssAnimation* animation = prCssAnimationFromEventTarget(static_cast<PrDomEventTarget*>(target));
    if (animation == nullptr)
        return 0;
    switch (prCssAnimationKind(animation)) {
        case PrCssAnimationKind_Css: return cssAnimationClass;
        case PrCssAnimationKind_Transition: return cssTransitionClass;
        default: return animationClass;
    }
}

JSClassID cssAnimationsEventClass(void* event) {
    return prCssAnimationPlaybackEventFromEvent(static_cast<PrDomEvent*>(event)) != nullptr ? playbackEventClass : 0;
}

} // namespace pr_impl
