// Animation values: how the animation elements read, interpolate and compose the values of an attribute

#include "svg/src/svg_animate_value.h"

#include "pr/ascii.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/layout_animation.h"
#include "pr/min_max.h"
#include "pr/path_data.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_property.h"
#include "css/src/css_rule.h"
#include "css/src/css_serialize.h"
#include "css/src/css_style.h"
#include "css/src/css_value.h"
#include "dom/src/dom_node.h"
#include "svg/src/svg_animate_names.h"
#include "svg/src/svg_attribute.h"
#include "svg/src/svg_types.h"

namespace pr_impl {

namespace {

// The unit of a component of a value: a length's or an angle's, or PrCssUnit_Number for a number alone.
typedef PrCssUnit Unit;

typedef SvgAnimateValue Operand;

bool has(const SvgAnimateFunction& function, SvgAnimateFlags flag) {
    return (function.flags & flag) != 0;
}

// Sets or clears `flag`.
void put(SvgAnimateFunction* function, uint32_t flag, bool on) {
    function->flags = SvgAnimateFlags(on ? function->flags | flag : function->flags & ~flag);
}

bool isIntegerType(PrSvgValueType type) {
    return type == PrSvgValueType_Integer || type == PrSvgValueType_IntegerPair;
}

bool isLengthType(PrSvgValueType type) {
    return type == PrSvgValueType_Length || type == PrSvgValueType_LengthList;
}

// Returns true if a value of the type may have `count` components.
bool countFits(PrSvgValueType type, uint32_t count) {
    switch (type) {
        case PrSvgValueType_Number:
        case PrSvgValueType_Integer:
        case PrSvgValueType_Length:
        case PrSvgValueType_Orient:
            return count == 1;
        case PrSvgValueType_NumberPair:
        case PrSvgValueType_IntegerPair:
            return count == 1 || count == 2;
        case PrSvgValueType_Rect:
            return count == 4;
        case PrSvgValueType_PointList:
            return count >= 2 && count % 2 == 0;
        default:
            return count >= 1;
    }
}

// Reads the unit that `text` spells for a component of the type: a length's, an angle's, or none for a number. Returns
// false if it spells none that the type takes.
bool unitOf(PrSvgValueType type, PrStringView text, Unit* out) {
    if (isLengthType(type))
        *out = svgLengthUnitOf(text);
    else if (type == PrSvgValueType_Orient)
        *out = svgAngleUnitOf(text);
    else
        *out = text.size == 0 ? PrCssUnit(PrCssUnit_Number) : PrCssUnit(PrCssUnit_Unknown);
    return *out != PrCssUnit_Unknown;
}

// Returns true if the context resolves the unit.
bool contextualUnit(PrSvgValueType type, Unit unit) {
    return isLengthType(type) && (unit == PrCssUnit_Percent || unit == PrCssUnit_Em || unit == PrCssUnit_Ex);
}

// Returns the extent that a percentage measures against.
double extentOf(const SvgAnimateContext& context) {
    switch (context.axis) {
        case SvgAxis_X: return context.width;
        case SvgAxis_Y: return context.height;
        default: return prF64Sqrt((context.width * context.width + context.height * context.height) / 2);
    }
}

// Returns how many user units, or degrees for an angle, one `unit` is for a component of the type.
double scaleOf(const SvgAnimateContext& context, PrSvgValueType type, Unit unit) {
    if (isLengthType(type))
        return svgLengthUnitScale(unit, PrSvgLengthBasis{float(extentOf(context)), float(context.fontSize)});
    return type == PrSvgValueType_Orient ? svgAngleUnitDegrees(unit) : 1;
}

// Returns `number` of `unit` in user units, or in degrees for an angle.
double resolveUnit(const SvgAnimateContext& context, PrSvgValueType type, Unit unit, double number) {
    return number * scaleOf(context, type, unit);
}

// Returns the number of `unit` that `resolved` user units are, or 0 if the unit measures a zero extent.
double unresolveUnit(const SvgAnimateContext& context, PrSvgValueType type, Unit unit, double resolved) {
    double factor = scaleOf(context, type, unit);
    return factor == 0 ? 0 : resolved / factor;
}

// Returns component `i` of a value in user units, or in degrees for an angle.
double resolvedAt(const SvgAnimateContext& context, const Operand& value, uint32_t i) {
    return resolveUnit(context, value.type, value.units.data[i], value.numbers.data[i]);
}

// Skips white space and at most one comma.
void skipSeparator(PrStringView text, size_t* at) {
    while (*at < text.size && prAsciiIsSpace(text.data[*at]))
        ++*at;
    if (*at < text.size && text.data[*at] == ',')
        ++*at;
    while (*at < text.size && prAsciiIsSpace(text.data[*at]))
        ++*at;
}

// Reads a number at `*at` and advances past it. Returns false if there is none.
bool readNumber(PrStringView text, size_t* at, double* out) {
    size_t read = prF64ParsePrefix(PrStringView{text.data + *at, text.size - *at}, out);
    if (read == 0 || !prF64IsFinite(*out))
        return false;
    *at += read;
    return true;
}

// Returns a color within sRGB's gamut, its alpha within [0, 1], with no component missing.
CssColor clamped(CssColor color) {
    for (double& channel : color.c)
        channel = clamp(channel, 0.0, 1.0);
    color.alpha = clamp(color.alpha, 0.0, 1.0);
    color.missing = 0;
    return color;
}

// Reads the color that `text` names in sRGB, which animations interpolate in (svg-animations/#ValueAttributes), with
// currentcolor resolved against the target, which makes the value contextual.
bool readColor(const SvgAnimateContext& context, PrStringView text, CssColor* out, bool* contextual) {
    PrCssUsedColor used{};
    if (prCssParseColor(text, context.target, &used) != PrStatus_Ok)
        return false;
    CssColor color{used.space, {used.components[0], used.components[1], used.components[2]}, used.alpha, used.missing};
    *out = clamped(cssColorConvert(color, PrColorSpace_Srgb));
    *contextual = *contextual || used.readsElement;
    return true;
}

// Returns the path data that the computed value of d quotes in path(), or `text` itself if it is not such a value.
PrStringView pathFunctionData(PrStringView text) {
    if (text.size < 8 || !prStringStartsWith(text, "path(") || text.data[text.size - 1] != ')')
        return text;
    PrStringView inner = prStringTrim(PrStringView{text.data + 5, text.size - 6});
    if (inner.size < 2 || (inner.data[0] != '"' && inner.data[0] != '\'') ||
            inner.data[inner.size - 1] != inner.data[0])
        return text;
    return PrStringView{inner.data + 1, inner.size - 2};
}

// The number of arguments that a function holds once those that it leaves out are filled in, by PrSvgTransformType.
constexpr uint32_t kTransformArguments[] = {0, 6, 2, 2, 3, 1, 1};

// Returns the function that a value animates as one of the functions of a transform list.
PrSvgTransform functionOf(const Operand& value) {
    PrSvgTransform out{.type = value.transform};
    for (uint32_t i = 0; i < value.numbers.count && i < 6; ++i)
        out.values[i] = float(value.numbers.data[i]);
    return out;
}

// Reads a transform list. A list of one function of animateTransform's kinds is that function, with the arguments that
// it leaves out filled in: a second translation of 0, a single scale for both axes, and a rotation about the origin.
// Another list is valid only as the list that an appending animateTransform follows.
bool readTransform(PrStringView text, Operand* out) {
    RawArray<PrSvgTransform> list;
    bool parsed = svgParseTransformList(text, &list);
    bool single = parsed && list.count == 1 && list.data[0].type != PrSvgTransformType_Matrix;
    if (single) {
        PrSvgTransformType type = list.data[0].type;
        for (uint32_t i = 0; i < kTransformArguments[type]; ++i) {
            out->numbers.push(double(list.data[0].values[i]));
            out->units.push(PrCssUnit_Number);
        }
        out->transform = type;
    } else if (parsed) {
        out->before.append(list.span());
    }
    list.reset();
    return single;
}

// Reads the components of a value of numbers, each with its unit. Reads a math function of a length at `*at`, such as
// calc(5% + 2px), in user units, and advances past it (css-values-4/#math). Returns false if there is none there, or it
// is not a length.
bool readMath(const SvgAnimateContext& context, PrStringView text, size_t* at, double* out) {
    size_t end = *at;
    while (end < text.size && prAsciiIsLetter(text.data[end]))
        ++end;
    if (end == *at || end == text.size || text.data[end] != '(')
        return false;
    uint32_t depth = 0;
    for (; end < text.size; ++end) {
        if (text.data[end] == '(')
            ++depth;
        else if (text.data[end] == ')' && --depth == 0)
            break;
    }
    if (end == text.size)
        return false;
    ++end;
    CssValuePool pool{};
    bool ok = cssParseDeclaredValue(cssPropertyDeclaredName(PrCssProperty_Width),
        PrStringView{text.data + *at, end - *at}, &pool, nullptr) && pool.values.count != 0 &&
        cssResolveLength(cssValuePoolRun(&pool, CssValueSpan{0, pool.values.count}), 0, extentOf(context), out);
    cssValuePoolReset(&pool);
    if (ok)
        *at = end;
    return ok;
}

void readNumbers(const SvgAnimateContext& context, PrSvgValueType type, PrStringView value, Operand* out) {
    size_t at = 0;
    skipSeparator(value, &at);
    while (at < value.size) {
        double number = 0;
        // A length may be a math function, which the context resolves in user units.
        if (isLengthType(type) && readMath(context, value, &at, &number)) {
            out->contextual = true;
            out->numbers.push(number);
            out->units.push(PrCssUnit_Number);
            skipSeparator(value, &at);
            continue;
        }
        if (!readNumber(value, &at, &number))
            return;
        size_t start = at;
        while (at < value.size && (value.data[at] == '%' || prAsciiIsLetter(value.data[at])))
            ++at;
        Unit unit = PrCssUnit_Number;
        if (!unitOf(type, PrStringView{value.data + start, at - start}, &unit))
            return;
        out->numbers.push(number);
        out->units.push(unit);
        skipSeparator(value, &at);
    }
    out->valid = countFits(type, out->numbers.count);
    // A number-optional-number written with one number is that number twice, so that it interpolates with a value of
    // two.
    if (out->valid && out->numbers.count == 1 &&
            (type == PrSvgValueType_NumberPair || type == PrSvgValueType_IntegerPair)) {
        double number = out->numbers.data[0];
        PrCssUnit unit = out->units.data[0];
        out->numbers.push(number);
        out->units.push(unit);
    }
}

// A value made by a combination: of the type, and with no text.
Operand made(PrSvgValueType type) {
    Operand out{};
    out.type = type;
    out.valid = true;
    return out;
}

// Replaces `out` with `from`, which it takes.
void replace(Operand* out, Operand* from) {
    svgAnimateValueReset(out);
    *out = *from;
    *from = Operand{};
}

// How two values combine.
enum {
    Combine_Interpolate,
    Combine_Add,
};
typedef uint8_t Combine;

double lerp(double from, double to, double progress) {
    if (progress <= 0)
        return from;
    if (progress >= 1)
        return to;
    return from + (to - from) * progress;
}

// Combines two values of numbers component by component. A pair of components in one unit keeps it. A pair in two units
// combines in user units: an interpolation writes the unit of the nearer value, the first at the midpoint, and a sum
// writes user units. Returns false if the values do not combine.
bool combineNumbers(const SvgAnimateContext& context, PrSvgValueType type, const Operand& a, const Operand& b,
        Combine op, double progress, Operand* out) {
    if (a.numbers.count == 0 || a.numbers.count != b.numbers.count || a.transform != b.transform)
        return false;
    Operand result = made(type);
    result.transform = a.transform;
    for (uint32_t i = 0; i < a.numbers.count; ++i) {
        bool shared = a.units.data[i] == b.units.data[i];
        double left = shared ? a.numbers.data[i] : resolvedAt(context, a, i);
        double right = shared ? b.numbers.data[i] : resolvedAt(context, b, i);
        Unit unit = a.units.data[i];
        if (!shared)
            unit = op == Combine_Add ? Unit(PrCssUnit_Number) : progress <= 0.5 ? a.units.data[i] : b.units.data[i];
        double number = op == Combine_Add ? left + right : lerp(left, right, progress);
        if (!shared && op != Combine_Add)
            number = unresolveUnit(context, type, unit, number);
        if (isIntegerType(type))
            number = prF64Floor(number + 0.5);
        result.numbers.push(number);
        result.units.push(unit);
    }
    replace(out, &result);
    return true;
}

// Combines two paths command by command, which requires the same commands (SVG2/paths.html#PathDataAnimation). An arc's
// flag is set if its combination is above zero.
bool combinePath(const Operand& a, const Operand& b, Combine op, double progress, Operand* out) {
    if (a.path.count != b.path.count)
        return false;
    Operand result = made(PrSvgValueType_Path);
    for (uint32_t i = 0; i < a.path.count; ++i) {
        const PrPathDataCommand& x = a.path.data[i];
        const PrPathDataCommand& y = b.path.data[i];
        if (x.letter != y.letter) {
            svgAnimateValueReset(&result);
            return false;
        }
        PrPathDataCommand command = x;
        uint32_t count = prPathDataArgumentCount(x.letter);
        for (uint32_t arg = 0; arg < count; ++arg) {
            double left = double(x.arguments[arg]);
            double right = double(y.arguments[arg]);
            bool flag = (x.letter == 'A' || x.letter == 'a') && (arg == 3 || arg == 4);
            double value = op == Combine_Add ? left + right : lerp(left, right, progress);
            command.arguments[arg] = float(flag ? (value > 0 ? 1 : 0) : value);
        }
        result.path.push(command);
    }
    replace(out, &result);
    return true;
}

// Combines two colors, each sRGB component and alpha on its own and kept within its range
// (svg-animations/#ValueAttributes).
bool combineColor(const Operand& a, const Operand& b, Combine op, double progress, Operand* out) {
    if (!a.hasColor || !b.hasColor)
        return false;
    Operand result = made(PrSvgValueType_Color);
    result.color.space = PrColorSpace_Srgb;
    for (uint32_t i = 0; i < 3; ++i)
        result.color.c[i] = op == Combine_Add ? a.color.c[i] + b.color.c[i] : lerp(a.color.c[i], b.color.c[i], progress);
    result.color.alpha = op == Combine_Add ? a.color.alpha + b.color.alpha : lerp(a.color.alpha, b.color.alpha, progress);
    result.color = clamped(result.color);
    result.hasColor = true;
    replace(out, &result);
    return true;
}

bool combine(const SvgAnimateContext& context, PrSvgValueType type, const Operand& a, const Operand& b, Combine op,
        double progress, Operand* out) {
    if (!a.valid || !b.valid)
        return false;
    switch (type) {
        case PrSvgValueType_String: return false;
        case PrSvgValueType_Color: return combineColor(a, b, op, progress, out);
        case PrSvgValueType_Path: return combinePath(a, b, op, progress, out);
        default: return combineNumbers(context, type, a, b, op, progress, out);
    }
}

// Writes to `out` the value scaled by a whole factor, which a cumulative repeat adds
// (svg-animations/#AccumulateAttribute).
bool scaleOperand(const SvgAnimateContext& context, PrSvgValueType type, const Operand& value, uint32_t factor, Operand* out) {
    if (!value.valid || type == PrSvgValueType_String)
        return false;
    if (type == PrSvgValueType_Color || type == PrSvgValueType_Path) {
        svgAnimateValueCopy(out, value);
        Operand sum{};
        bool ok = true;
        for (uint32_t i = 1; i < factor && ok; ++i) {
            ok = combine(context, type, *out, value, Combine_Add, 0, &sum);
            if (ok)
                replace(out, &sum);
        }
        svgAnimateValueReset(&sum);
        return ok;
    }
    Operand result = made(type);
    result.transform = value.transform;
    for (uint32_t i = 0; i < value.numbers.count; ++i) {
        result.numbers.push(value.numbers.data[i] * factor);
        result.units.push(value.units.data[i]);
    }
    replace(out, &result);
    return true;
}

// Returns true if the value has no component that an addition could take, as empty path data has none.
bool addsNothing(const Operand& value) {
    return value.numbers.count == 0 && value.path.count == 0 && !value.hasColor;
}

// Writes to `out` the value of the same shape with every component zero, which a by animation starts from
// (svg-animations/#ByAttribute). A color's zero is transparent black.
void zeroLike(const Operand& value, Operand* out) {
    Operand result = made(value.type);
    result.transform = value.transform;
    result.valid = value.valid;
    result.hasColor = value.hasColor;
    for (uint32_t i = 0; i < value.path.count; ++i) {
        PrPathDataCommand command = value.path.data[i];
        for (float& argument : command.arguments)
            argument = 0;
        result.path.push(command);
    }
    for (uint32_t i = 0; i < value.numbers.count; ++i) {
        result.numbers.push(0);
        result.units.push(value.units.data[i]);
    }
    replace(out, &result);
}

// Returns the distance between two values, which paced animation spaces its intervals by
// (svg-animations/#CalcModeAttribute), or -1 if the type has no distance.
double distanceOf(const SvgAnimateContext& context, PrSvgValueType type, const Operand& a, const Operand& b) {
    if (!a.valid || !b.valid)
        return -1;
    double sum = 0;
    if (type == PrSvgValueType_Color) {
        for (uint32_t i = 0; i < 3; ++i)
            sum += (a.color.c[i] - b.color.c[i]) * (a.color.c[i] - b.color.c[i]);
        return prF64Sqrt(sum);
    }
    if (type == PrSvgValueType_String || type == PrSvgValueType_Path || a.numbers.count != b.numbers.count)
        return -1;
    for (uint32_t i = 0; i < a.numbers.count; ++i) {
        double delta = resolvedAt(context, a, i) - resolvedAt(context, b, i);
        sum += delta * delta;
    }
    return prF64Sqrt(sum);
}

SvgCalcMode calcModeOf(PrStringView text, PrSvgValueType type) {
    PrStringView named = prStringTrim(text);
    SvgCalcMode mode = SvgCalcMode_Linear;
    if (named == "discrete")
        mode = SvgCalcMode_Discrete;
    else if (named == "paced")
        mode = SvgCalcMode_Paced;
    else if (named == "spline")
        mode = SvgCalcMode_Spline;
    // A type that does not interpolate animates discretely, whatever the mode.
    return type == PrSvgValueType_String ? SvgCalcMode(SvgCalcMode_Discrete) : mode;
}

// Appends a value read from `text`, and returns it. A value whose reading depends on its context makes the function
// contextual.
Operand& addValue(SvgAnimateFunction* function, const SvgAnimateContext& context, PrSvgValueType type, PrStringView text) {
    Operand& value = function->values.emplace();
    value = Operand{};
    svgAnimateValueRead(context, type, text, &value);
    if (value.contextual)
        put(function, SvgAnimateFlag_Contextual, true);
    return value;
}

// Fills in the key times that a function without keyTimes has: one for each value, evenly spaced.
void evenKeyTimes(SvgAnimateFunction* function) {
    uint32_t count = function->values.count;
    for (uint32_t i = 0; i < count; ++i) {
        if (function->calcMode == SvgCalcMode_Discrete)
            function->keyTimes.push(double(i) / count);
        else
            function->keyTimes.push(count == 1 ? 0 : double(i) / (count - 1));
    }
}

// Fills in the key times of paced animation, spaced by the distance between the values. Returns false if the type has
// no distance. Values in units that the context resolves make the function contextual.
bool pacedKeyTimes(const SvgAnimateContext& context, SvgAnimateFunction* function) {
    RawArray<double> spans;
    double total = 0;
    bool ok = true;
    for (uint32_t i = 0; i + 1 < function->values.count && ok; ++i) {
        double span = distanceOf(context, function->type, function->values.data[i], function->values.data[i + 1]);
        ok = span >= 0;
        spans.push(span);
        total += span;
    }
    ok = ok && total > 0;
    double walked = 0;
    for (uint32_t i = 0; ok && i < function->values.count; ++i) {
        function->keyTimes.push(walked / total);
        if (i < spans.count)
            walked += spans.data[i];
        const Operand& value = function->values.data[i];
        for (uint32_t j = 0; j < value.units.count; ++j) {
            if (contextualUnit(value.type, value.units.data[j]))
                put(function, SvgAnimateFlag_Contextual, true);
        }
    }
    spans.reset();
    return ok;
}

// Builds the function of the values of `spec`, each read as `type`.
bool compileValues(const SvgAnimateContext& context, const SvgAnimateSpec& spec, PrSvgValueType type,
        SvgAnimateFunction* out) {
    SvgAnimateFunction& f = *out;
    f.type = type;
    f.calcMode = calcModeOf(spec.calcMode, type);
    put(&f, SvgAnimateFlag_Additive, prStringTrim(spec.additive) == "sum");
    put(&f, SvgAnimateFlag_Accumulate, prStringTrim(spec.accumulate) == "sum");
    put(&f, SvgAnimateFlag_Set, spec.set);
    if (spec.set) {
        if (spec.to.data == nullptr)
            return false;
        // A set writes its value as written, whatever the type.
        f.type = PrSvgValueType_String;
        f.calcMode = SvgCalcMode_Discrete;
        addValue(&f, context, PrSvgValueType_String, spec.to);
        f.keyTimes.push(0);
        put(&f, SvgAnimateFlag_Additive | SvgAnimateFlag_Accumulate, false);
        return true;
    }
    RawArray<PrStringView> items;
    if (spec.values.data != nullptr)
        svgAnimateSplit(spec.values, &items);
    bool ok = true;
    if (spec.values.data != nullptr) {
        // A values attribute overrides from, to and by. One with no values is in error.
        ok = items.count != 0;
        for (uint32_t i = 0; i < items.count && ok; ++i)
            ok = addValue(&f, context, type, items.data[i]).valid;
    } else if (spec.from.data != nullptr && spec.to.data != nullptr) {
        ok = addValue(&f, context, type, spec.from).valid && addValue(&f, context, type, spec.to).valid;
    } else if (spec.from.data != nullptr && spec.by.data != nullptr) {
        // A from-by animation ends at the sum of from and by.
        Operand by{};
        svgAnimateValueRead(context, type, spec.by, &by);
        if (by.contextual)
            put(&f, SvgAnimateFlag_Contextual, true);
        Operand& from = addValue(&f, context, type, spec.from);
        Operand sum{};
        ok = from.valid && !addsNothing(by) && combine(context, type, from, by, Combine_Add, 0, &sum);
        if (ok)
            f.values.push(sum);
        else
            svgAnimateValueReset(&sum);
        svgAnimateValueReset(&by);
    } else if (spec.to.data != nullptr) {
        // A to animation starts at the underlying value, and neither adds to it nor accumulates
        // (svg-animations/#ToAttribute).
        Operand& first = f.values.emplace();
        first = made(type);
        ok = addValue(&f, context, type, spec.to).valid;
        put(&f, SvgAnimateFlag_ToAnimation, true);
        put(&f, SvgAnimateFlag_Additive | SvgAnimateFlag_Accumulate, false);
    } else if (spec.by.data != nullptr) {
        // A by animation adds a delta that runs from zero to by. A type that does not add has none.
        Operand& by = addValue(&f, context, type, spec.by);
        ok = by.valid && !addsNothing(by) && type != PrSvgValueType_String;
        if (ok) {
            Operand zero{};
            zeroLike(by, &zero);
            f.values.insert(f.values.count - 1, zero);
            put(&f, SvgAnimateFlag_Additive, true);
        }
    } else {
        ok = false;
    }
    items.reset();
    if (!ok)
        return false;
    if (f.calcMode == SvgCalcMode_Paced && !pacedKeyTimes(context, &f)) {
        f.keyTimes.clear();
        f.calcMode = SvgCalcMode_Linear;
    }
    if (f.keyTimes.count == 0) {
        if (spec.keyTimes.data != nullptr && prStringTrim(spec.keyTimes).size != 0 && f.calcMode != SvgCalcMode_Paced) {
            if (!svgAnimateKeyTimes(spec.keyTimes, f.values.count, f.calcMode == SvgCalcMode_Discrete, &f.keyTimes))
                return false;
        } else {
            evenKeyTimes(&f);
        }
    }
    return f.calcMode != SvgCalcMode_Spline || svgAnimateKeySplines(spec.keySplines, f.values.count - 1, &f.keySplines);
}

// Builds the function of an animateTransform. Its values are the arguments of the transform function that `type` names,
// and the animation function reads each as that whole transform function (svg-animations/#AnimateTransformElement).
bool compileTransform(const SvgAnimateContext& context, const SvgAnimateSpec& spec, SvgAnimateFunction* out) {
    PrStringView named = prStringTrim(spec.transformType);
    // An animateTransform animates any transform function except matrix.
    PrSvgTransformType kind = PrSvgTransformType_Translate;
    if (named.size != 0)
        kind = svgKeywordValue(SvgKeywords_TransformType, named);
    if (kind == 0 || kind == PrSvgTransformType_Matrix)
        return false;
    PrTextBuffer texts[4] = {};
    PrStringView stated[4] = {spec.values, spec.from, spec.to, spec.by};
    PrStringView written[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        if (stated[i].data == nullptr)
            continue;
        RawArray<PrStringView> items;
        svgAnimateSplit(stated[i], &items);
        for (uint32_t j = 0; j < items.count; ++j) {
            if (j != 0)
                prTextBufferAppendChar(&texts[i], ';');
            prTextBufferAppend(&texts[i], svgKeywordName(SvgKeywords_TransformType, kind));
            prTextBufferAppendChar(&texts[i], '(');
            prTextBufferAppend(&texts[i], items.data[j]);
            prTextBufferAppendChar(&texts[i], ')');
        }
        items.reset();
        written[i] = prTextBufferView(&texts[i]);
        if (written[i].data == nullptr)
            written[i] = "";
    }
    SvgAnimateSpec functions = spec;
    functions.set = false;
    functions.values = written[0];
    functions.from = written[1];
    functions.to = written[2];
    functions.by = written[3];
    bool ok = compileValues(context, functions, PrSvgValueType_TransformList, out);
    // An additive animateTransform appends its function to the underlying list, rather than adding arguments.
    put(out, SvgAnimateFlag_Appends, has(*out, SvgAnimateFlag_Additive));
    put(out, SvgAnimateFlag_Additive, false);
    out->transform = kind;
    // A value writes its function from its arguments, as the text that it was read from is the arguments alone.
    for (uint32_t i = 0; i < out->values.count; ++i)
        prRawStringReset(&out->values.data[i].text);
    for (PrTextBuffer& text : texts)
        prTextBufferReset(&text);
    return ok;
}

// Returns the interval that `progress` falls in, and writes the progress through it.
uint32_t intervalAt(const SvgAnimateFunction& function, double progress, double* local) {
    const RawArray<double>& times = function.keyTimes;
    *local = 0;
    uint32_t at = 0;
    if (function.calcMode == SvgCalcMode_Discrete) {
        while (at + 1 < times.count && times.data[at + 1] <= progress)
            ++at;
        return at;
    }
    while (at + 2 < times.count && times.data[at + 1] <= progress)
        ++at;
    double span = times.data[at + 1] - times.data[at];
    *local = span <= 0 ? 0 : (progress - times.data[at]) / span;
    return at;
}

// Writes the underlying value as the function's type to `out`: itself, or read again from its text if it is of another
// type.
void underlyingAs(const SvgAnimateContext& context, PrSvgValueType type, const Operand& underlying, Operand* out) {
    if (underlying.type == type) {
        svgAnimateValueCopy(out, underlying);
        return;
    }
    PrTextBuffer text{};
    svgAnimateValueWrite(underlying, &text);
    svgAnimateValueRead(context, type, prTextBufferView(&text), out);
    prTextBufferReset(&text);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Values

void svgAnimateValueReset(SvgAnimateValue* value) {
    value->numbers.reset();
    value->units.reset();
    value->path.reset();
    value->before.reset();
    prRawStringReset(&value->text);
    *value = SvgAnimateValue{};
}

void svgAnimateValueCopy(SvgAnimateValue* out, const SvgAnimateValue& from) {
    if (out == &from)
        return;
    svgAnimateValueReset(out);
    out->numbers.append(from.numbers.span());
    out->units.append(from.units.span());
    out->path.append(from.path.span());
    out->before.append(from.before.span());
    out->text = prRawStringClone(&from.text);
    out->color = from.color;
    out->type = from.type;
    out->transform = from.transform;
    out->hasColor = from.hasColor;
    out->valid = from.valid;
    out->contextual = from.contextual;
}

bool svgAnimateValueEqual(const SvgAnimateValue& a, const SvgAnimateValue& b) {
    if (a.type != b.type || a.valid != b.valid || a.transform != b.transform || a.hasColor != b.hasColor ||
            prRawStringIsNull(&a.text) != prRawStringIsNull(&b.text) ||
            prRawStringView(&a.text) != prRawStringView(&b.text) ||
            a.before.count != b.before.count || a.numbers.count != b.numbers.count || a.path.count != b.path.count)
        return false;
    for (uint32_t i = 0; i < a.before.count; ++i) {
        if (a.before.data[i].type != b.before.data[i].type)
            return false;
        for (uint32_t k = 0; k < 6; ++k) {
            if (a.before.data[i].values[k] != b.before.data[i].values[k])
                return false;
        }
    }
    for (uint32_t i = 0; i < a.numbers.count; ++i) {
        if (a.numbers.data[i] != b.numbers.data[i] || a.units.data[i] != b.units.data[i])
            return false;
    }
    for (uint32_t i = 0; i < a.path.count; ++i) {
        const PrPathDataCommand& x = a.path.data[i];
        const PrPathDataCommand& y = b.path.data[i];
        if (x.letter != y.letter)
            return false;
        for (uint32_t arg = 0; arg < prPathDataArgumentCount(x.letter); ++arg) {
            if (x.arguments[arg] != y.arguments[arg])
                return false;
        }
    }
    if (a.hasColor && (a.color.c[0] != b.color.c[0] || a.color.c[1] != b.color.c[1] || a.color.c[2] != b.color.c[2] ||
            a.color.alpha != b.color.alpha))
        return false;
    return true;
}

void svgAnimateValueRead(const SvgAnimateContext& context, PrSvgValueType type, PrStringView text, SvgAnimateValue* out) {
    svgAnimateValueReset(out);
    out->type = type;
    PrStringView trimmed = prStringTrim(text);
    // A presentation attribute's inherit is the value of the target's parent, as the presentation last styled it
    // (svg-animations/#FromAttribute).
    PrTextBuffer inherited{};
    const PrDomElement* parent = context.target != nullptr ? domElementFromNode(context.target->parent) : nullptr;
    PrCssPresentation* presentation = parent != nullptr ? cssPresentationOf(parent->document) : nullptr;
    if (prAsciiEqualsFold(trimmed, "inherit") && context.property != PrCssProperty_Count && presentation != nullptr &&
            cssPresentationStyledValue(presentation, parent, context.property, &inherited)) {
        trimmed = prStringTrim(prTextBufferView(&inherited));
        out->contextual = true;
    }
    out->text = prRawStringCopy(trimmed.data != nullptr ? trimmed : PrStringView(""));
    prTextBufferReset(&inherited);
    PrStringView value = prRawStringView(&out->text);
    switch (type) {
        case PrSvgValueType_String:
            out->valid = true;
            return;
        case PrSvgValueType_Color:
            out->hasColor = readColor(context, value, &out->color, &out->contextual);
            out->valid = out->hasColor;
            return;
        case PrSvgValueType_Path: {
            // Empty path data is a value of its own, which combines only with another.
            PrStringView data = prStringTrim(pathFunctionData(value));
            uint32_t count = prPathDataParse(data, nullptr, 0);
            out->path.resize(count);
            prPathDataParse(data, out->path.data, count);
            prPathDataMakeAbsolute(out->path.data, count);
            out->valid = data.size == 0 || count != 0;
            return;
        }
        case PrSvgValueType_TransformList:
            out->valid = readTransform(value, out);
            return;
        default:
            break;
    }
    // An orientation is an angle or one of two keywords, which have no components (SVG2/painting.html#OrientAttribute).
    if (type == PrSvgValueType_Orient && (value == "auto" || value == "auto-start-reverse")) {
        out->valid = true;
        return;
    }
    readNumbers(context, type, value, out);
}

void svgAnimateValueWrite(const SvgAnimateValue& value, PrTextBuffer* out) {
    if (!prRawStringIsNull(&value.text)) {
        prTextBufferAppend(out, prRawStringView(&value.text));
        return;
    }
    if (value.type == PrSvgValueType_Color) {
        const CssColor& c = value.color;
        PrCssUsedColor color{
            .space = PrColorSpace_Srgb,
            .legacy = true,
            .components = {c.c[0], c.c[1], c.c[2]},
            .alpha = c.alpha,
        };
        PrRawString text = prCssUsedColorSerialize(&color, PrCssColorSerialization_Css);
        prTextBufferAppend(out, prRawStringView(&text));
        prRawStringReset(&text);
        return;
    }
    if (value.type == PrSvgValueType_Path) {
        PrRawString text{};
        prPathDataWrite(value.path.data, value.path.count, &text);
        prTextBufferAppend(out, prRawStringView(&text));
        prRawStringReset(&text);
        return;
    }
    bool transform = value.type == PrSvgValueType_TransformList;
    if (transform) {
        for (uint32_t i = 0; i < value.before.count; ++i) {
            if (i != 0)
                prTextBufferAppendChar(out, ' ');
            svgAppendTransform(out, value.before.data[i]);
        }
        if (value.transform == 0)
            return;
        if (value.before.count != 0)
            prTextBufferAppendChar(out, ' ');
        prTextBufferAppend(out, svgKeywordName(SvgKeywords_TransformType, value.transform));
        prTextBufferAppendChar(out, '(');
    }
    for (uint32_t i = 0; i < value.numbers.count; ++i) {
        if (i != 0)
            prTextBufferAppendChar(out, ' ');
        cssSerializeNumber(out, value.numbers.data[i]);
        prTextBufferAppend(out, svgUnitName(value.units.data[i]));
    }
    if (transform)
        prTextBufferAppendChar(out, ')');
}

void svgAnimateTransforms(const SvgAnimateValue& value, RawArray<PrSvgTransform>* out) {
    out->append(value.before.span());
    if (value.transform != 0)
        out->push(functionOf(value));
}

// ---------------------------------------------------------------------------------------------------------------------
// Functions

bool svgAnimateCompile(const SvgAnimateContext& context, const SvgAnimateSpec& spec, SvgAnimateFunction* out) {
    svgAnimateFunctionReset(out);
    if (spec.transformType.data != nullptr && !spec.set)
        return compileTransform(context, spec, out);
    return compileValues(context, spec, spec.type, out);
}

void svgAnimateFunctionReset(SvgAnimateFunction* function) {
    for (uint32_t i = 0; i < function->values.count; ++i)
        svgAnimateValueReset(&function->values.data[i]);
    function->values.reset();
    function->keyTimes.reset();
    function->keySplines.reset();
    *function = SvgAnimateFunction{};
}

bool svgAnimateSample(const SvgAnimateContext& context, const SvgAnimateFunction& function,
        const SvgAnimateValue& underlying, double progress, uint32_t repeat, SvgAnimateValue* out) {
    const SvgAnimateFunction& f = function;
    double clamped = clamp(progress, 0.0, 1.0);
    bool transform = f.transform != 0;
    Operand base{};
    if (transform || (f.flags & (SvgAnimateFlag_ToAnimation | SvgAnimateFlag_Additive)) != 0)
        underlyingAs(context, f.type, underlying, &base);
    // An animateTransform starts a to animation at an underlying value of the same single function, and appends its
    // function to the whole underlying list. The transform property's none is an empty list.
    RawArray<PrSvgTransform> before;
    if (transform) {
        if (has(f, SvgAnimateFlag_Appends)) {
            before.append(base.before.span());
            if (base.transform != 0)
                before.push(functionOf(base));
        }
        if (base.transform != f.transform || base.before.count != 0)
            base.valid = false;
    }
    Operand value{};
    bool ok = true;
    bool toAnimation = has(f, SvgAnimateFlag_ToAnimation);
    if (has(f, SvgAnimateFlag_Set) || f.values.count == 1) {
        svgAnimateValueCopy(&value, f.values.data[0]);
    } else {
        double local = 0;
        uint32_t at = intervalAt(f, clamped, &local);
        // A to animation starts at the underlying value of this sample, or at its to value if the target has none that
        // reads as the type.
        const Operand& from = !toAnimation || at != 0 ? f.values.data[at] : base.valid ? base : f.values.data[1];
        const Operand& to = f.values.data[at + 1 < f.values.count ? at + 1 : at];
        if (f.calcMode == SvgCalcMode_Discrete) {
            svgAnimateValueCopy(&value, from);
        } else {
            if (f.calcMode == SvgCalcMode_Spline)
                local = svgAnimateSpline(f.keySplines.data + at * 4, local);
            if (!combine(context, f.type, from, to, Combine_Interpolate, local, &value))
                svgAnimateValueCopy(&value, local < 0.5 ? from : to);
        }
    }
    if (has(f, SvgAnimateFlag_Accumulate) && repeat != 0) {
        Operand scaled{};
        Operand summed{};
        if (scaleOperand(context, f.type, f.values.last(), repeat, &scaled) &&
                combine(context, f.type, value, scaled, Combine_Add, 0, &summed))
            replace(&value, &summed);
        svgAnimateValueReset(&scaled);
        svgAnimateValueReset(&summed);
    }
    if (has(f, SvgAnimateFlag_Additive) && base.valid) {
        // An additive animation whose value the underlying value cannot take is in error.
        Operand summed{};
        ok = combine(context, f.type, base, value, Combine_Add, 0, &summed);
        if (ok)
            replace(&value, &summed);
        svgAnimateValueReset(&summed);
    }
    ok = ok && value.valid;
    if (ok && transform) {
        // The value writes its function from its arguments, after the list that it appends to.
        prRawStringReset(&value.text);
        value.before.reset();
        value.before = before;
        before = RawArray<PrSvgTransform>{};
    }
    if (ok)
        replace(out, &value);
    before.reset();
    svgAnimateValueReset(&value);
    svgAnimateValueReset(&base);
    return ok;
}

void svgAnimateSplit(PrStringView text, RawArray<PrStringView>* out) {
    uint32_t first = out->count;
    size_t start = 0;
    for (size_t i = 0; i <= text.size; ++i) {
        if (i != text.size && text.data[i] != ';')
            continue;
        out->push(prStringTrim(PrStringView{text.data + start, i - start}));
        start = i + 1;
    }
    if (out->count > first + 1 && out->last().size == 0)
        --out->count;
}

bool svgAnimateNumbers(PrStringView text, RawArray<double>* out) {
    size_t at = 0;
    skipSeparator(text, &at);
    while (at < text.size) {
        double number = 0;
        if (!readNumber(text, &at, &number))
            return false;
        out->push(number);
        skipSeparator(text, &at);
    }
    return true;
}

bool svgAnimateKeyTimes(PrStringView text, uint32_t values, bool discrete, RawArray<double>* out) {
    RawArray<PrStringView> items;
    svgAnimateSplit(text, &items);
    uint32_t first = out->count;
    bool ok = true;
    for (uint32_t i = 0; i < items.count && ok; ++i) {
        RawArray<double> one;
        ok = svgAnimateNumbers(items.data[i], &one) && one.count == 1;
        if (ok)
            out->push(one.data[0]);
        one.reset();
    }
    items.reset();
    uint32_t count = out->count - first;
    const double* times = out->data + first;
    ok = ok && count == values && count != 0 && times[0] == 0;
    for (uint32_t i = 0; ok && i < count; ++i)
        ok = times[i] >= 0 && times[i] <= 1 && (i == 0 || times[i] >= times[i - 1]);
    return ok && (discrete || count == 1 || times[count - 1] == 1);
}

bool svgAnimateKeySplines(PrStringView text, uint32_t intervals, RawArray<double>* out) {
    RawArray<PrStringView> sets;
    svgAnimateSplit(text, &sets);
    uint32_t first = out->count;
    bool ok = true;
    for (uint32_t i = 0; i < sets.count && ok; ++i) {
        RawArray<double> numbers;
        ok = svgAnimateNumbers(sets.data[i], &numbers) && numbers.count == 4;
        if (ok)
            out->append(numbers.data, numbers.count);
        numbers.reset();
    }
    sets.reset();
    ok = ok && out->count - first == intervals * 4;
    for (uint32_t i = first; ok && i < out->count; ++i)
        ok = out->data[i] >= 0 && out->data[i] <= 1;
    return ok;
}

double svgAnimateSpline(const double* spline, double x) {
    PrLayoutEasing easing = prLayoutEasingLinear();
    easing.kind = PrLayoutEasingKind_CubicBezier;
    easing.x1 = spline[0];
    easing.y1 = spline[1];
    easing.x2 = spline[2];
    easing.y2 = spline[3];
    return prLayoutEase(&easing, x, false);
}

} // namespace pr_impl
