#include "css/src/css_media.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"

#include "css/src/css_parse.h"
#include "css/src/css_parser.h"
#include "css/src/css_unit.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Values

CssMediaValue mediaNumber(double number) {
    return CssMediaValue{number, PrCssKeyword_Unknown};
}

CssMediaValue mediaKeyword(PrCssKeyword keyword) {
    return CssMediaValue{0, keyword};
}

CssMediaValue* valueOf(CssMediaEnvironment* environment, PrCssDescriptor descriptor) {
    return &environment->features[descriptor - kCssDescriptorMediaFirst];
}

// Returns the value that a query sees: the override of a preference if it has one, and otherwise the stored value.
const CssMediaValue* valueOf(const CssMediaEnvironment* environment, PrCssDescriptor descriptor) {
    const CssMediaValue* overridden =
        &environment->overrides[descriptor - kCssDescriptorMediaFirst];
    return overridden->keyword != PrCssKeyword_Unknown
               ? overridden
               : &environment->features[descriptor - kCssDescriptorMediaFirst];
}

// Returns true if the feature's value is computed from other features rather than stored.
bool isComputed(PrCssDescriptor descriptor) {
    switch (descriptor) {
        case PrCssDescriptor_MediaAspectRatio:
        case PrCssDescriptor_MediaDeviceAspectRatio:
        case PrCssDescriptor_MediaOrientation:
        case PrCssDescriptor_MediaWebkitDevicePixelRatio:
            return true;
        default:
            return false;
    }
}

// Returns the feature's value in the environment.
CssMediaValue featureValue(const CssMediaEnvironment* environment, PrCssDescriptor descriptor) {
    double width = valueOf(environment, PrCssDescriptor_MediaWidth)->number;
    double height = valueOf(environment, PrCssDescriptor_MediaHeight)->number;
    switch (descriptor) {
        case PrCssDescriptor_MediaAspectRatio:
            return mediaNumber(width / height);
        case PrCssDescriptor_MediaDeviceAspectRatio:
            return mediaNumber(valueOf(environment, PrCssDescriptor_MediaDeviceWidth)->number
                               / valueOf(environment, PrCssDescriptor_MediaDeviceHeight)->number);
        case PrCssDescriptor_MediaOrientation:
            // mediaqueries-5/#valdef-media-orientation-portrait
            return mediaKeyword(height >= width ? PrCssKeyword_Portrait : PrCssKeyword_Landscape);
        case PrCssDescriptor_MediaWebkitDevicePixelRatio:
            return *valueOf(environment, PrCssDescriptor_MediaResolution);
        default:
            return *valueOf(environment, descriptor);
    }
}

// Computes the px value of one `unit` of a relative length (mediaqueries-5/#units). Font-relative lengths resolve
// against the initial font. They use the x-height and character advances that css-values-4/#font-relative-lengths
// assumes when these cannot be measured. Viewport-percentage lengths resolve against the viewport. Container lengths
// fall back to the viewport too (css-conditional-5/#container-lengths). Returns false for a cap height or a line
// height, and for a unit that is not a relative length.
bool relativeLength(const CssMediaEnvironment* environment, PrCssUnit unit, double* px) {
    double em = environment->fontSize;
    double width = valueOf(environment, PrCssDescriptor_MediaWidth)->number / 100;
    double height = valueOf(environment, PrCssDescriptor_MediaHeight)->number / 100;
    switch (unit) {
        case PrCssUnit_Em:
        case PrCssUnit_Rem:
        case PrCssUnit_Ic:
        case PrCssUnit_Ric:
            *px = em;
            return true;
        case PrCssUnit_Ex:
        case PrCssUnit_Rex:
        case PrCssUnit_Ch:
        case PrCssUnit_Rch:
            *px = em / 2;
            return true;
        case PrCssUnit_Vw: case PrCssUnit_Svw: case PrCssUnit_Lvw: case PrCssUnit_Dvw:
        case PrCssUnit_Vi: case PrCssUnit_Svi: case PrCssUnit_Lvi: case PrCssUnit_Dvi:
        case PrCssUnit_Cqw: case PrCssUnit_Cqi:
            *px = width;
            return true;
        case PrCssUnit_Vh: case PrCssUnit_Svh: case PrCssUnit_Lvh: case PrCssUnit_Dvh:
        case PrCssUnit_Vb: case PrCssUnit_Svb: case PrCssUnit_Lvb: case PrCssUnit_Dvb:
        case PrCssUnit_Cqh: case PrCssUnit_Cqb:
            *px = height;
            return true;
        case PrCssUnit_Vmin:
        case PrCssUnit_Svmin:
        case PrCssUnit_Lvmin:
        case PrCssUnit_Dvmin:
        case PrCssUnit_Cqmin: *px = width < height ? width : height; return true;
        case PrCssUnit_Vmax:
        case PrCssUnit_Svmax:
        case PrCssUnit_Lvmax:
        case PrCssUnit_Dvmax:
        case PrCssUnit_Cqmax: *px = width > height ? width : height; return true;
        default:
            return false;
    }
}

// Computes the number that the value at `index` represents, in the canonical unit of its type. The value is a number, a
// dimension, or a node of a calculation tree. Returns false for a percentage, for a function left in a tree, and for a
// length that the environment gives no px value for.
bool calculate(CssValueRun run, uint32_t index, const CssMediaEnvironment* environment,
               double* out) {
    const CssValueNode* value = &run.values[index];
    switch (value->kind) {
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:
            *out = value->number;
            return true;
        case PrCssValueKind_Dimension: {
            double scale = cssUnitScale(value->unit);
            if (scale == 0 && !relativeLength(environment, value->unit, &scale))
                return false;
            *out = value->number * scale;
            return true;
        }
        case PrCssValueKind_Math:
            return calculate(run, index + 1, environment, out);
        case PrCssValueKind_Calc: {
            PrCssCalc op = cssValueCalcOperator(value);
            double result = op == PrCssCalc_Product ? 1 : 0;
            for (uint32_t at = index + 1; at < index + value->next; at += run.values[at].next) {
                double operand;
                if (!calculate(run, at, environment, &operand))
                    return false;
                switch (op) {
                    case PrCssCalc_Sum:     result += operand; break;
                    case PrCssCalc_Product: result *= operand; break;
                    case PrCssCalc_Negate:  result = -operand; break;
                    case PrCssCalc_Invert:  result = 1 / operand; break;
                }
            }
            *out = result;
            return true;
        }
        default:
            return false;
    }
}

// Computes the value that a match of a feature's grammar represents, from the values in [first, end): a keyword, or a
// number, a dimension or a ratio (css-values-4/#ratio). Returns false if one of its numbers is unknown.
bool mediaValue(CssValueRun run, uint32_t first, uint32_t end,
                const CssMediaEnvironment* environment, CssMediaValue* out) {
    const CssValueNode* value = &run.values[first];
    if (value->kind == PrCssValueKind_Keyword) {
        // mediaqueries-5/#valdef-media-resolution-infinite
        *out = cssValueKeyword(value) == PrCssKeyword_Infinite ? mediaNumber(kInfinityF64)
                                                   : mediaKeyword(cssValueKeyword(value));
        return true;
    }
    double number;
    if (!calculate(run, first, environment, &number))
        return false;
    uint32_t slash = first + value->next;
    if (slash < end) {
        double denominator;
        if (!calculate(run, slash + 1, environment, &denominator))
            return false;
        number /= denominator;
    }
    *out = mediaNumber(number);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Evaluation (mediaqueries-5/#evaluating)

// A three-valued truth, ordered so that `and` is the least of its operands and `or` the greatest.
enum {
    Truth_False,
    Truth_Unknown,
    Truth_True,
};
typedef uint8_t Truth;

Truth truthOf(bool value) {
    return value ? Truth_True : Truth_False;
}

Truth negated(Truth truth) {
    return Truth(Truth_True - truth);
}

Truth both(Truth a, Truth b) {
    return a < b ? a : b;
}

Truth either(Truth a, Truth b) {
    return a > b ? a : b;
}

// The orders of a feature's value relative to a query's value that a comparison accepts.
enum {
    Order_Less = 1 << 0,
    Order_Equal = 1 << 1,
    Order_Greater = 1 << 2,
};
typedef uint8_t Order;

// Returns the same comparison written with its operands on the other sides.
Order flipped(Order order) {
    Order result = order & Order_Equal;
    if ((order & Order_Less) != 0)
        result |= Order_Greater;
    if ((order & Order_Greater) != 0)
        result |= Order_Less;
    return result;
}

// Returns the comparison at `index`: an <mf-lt>, <mf-gt> or <mf-eq>, in which '<', '>' and '=' mean less, greater and
// equal.
Order comparison(CssValueRun run, uint32_t index) {
    Order order = 0;
    for (uint32_t at = index + 1; at < index + run.values[index].next; ++at) {
        switch (cssValueLiteral(&run.values[at])) {
            case '<': order |= Order_Less; break;
            case '>': order |= Order_Greater; break;
            default:  order |= Order_Equal; break;
        }
    }
    return order;
}

// Returns whether the feature's value `held` compares with `given` in an order that `order` accepts. Keywords are
// either equal or not comparable. A value that the environment does not have is unknown.
Truth compare(CssMediaValue held, CssMediaValue given, Order order) {
    if (held.keyword == PrCssKeyword_Unknown && isNanF64(held.number))
        return Truth_Unknown;
    if (held.keyword != PrCssKeyword_Unknown || given.keyword != PrCssKeyword_Unknown)
        return truthOf((order & Order_Equal) != 0 && held.keyword == given.keyword);
    Order found = 0;
    if (held.number < given.number)
        found = Order_Less;
    else if (held.number > given.number)
        found = Order_Greater;
    else if (held.number == given.number)
        found = Order_Equal;
    return truthOf((found & order) != 0);
}

// Returns the feature that the ident at `index` names. The ident is the child of an <mf-name>.
PrCssDescriptor featureOf(CssValueRun run, uint32_t index) {
    return cssValueFeature(&run.values[index]);
}

// Evaluates an <mf-boolean> whose <mf-name> has its ident at `index` (mediaqueries-5/#mq-boolean-context). The result
// is false for zero, for none, and for no-preference. Each feature that accepts none or no-preference defines that
// value as false in a boolean context.
Truth booleanFeature(CssValueRun run, uint32_t index, const CssMediaEnvironment* environment) {
    PrCssDescriptor descriptor = featureOf(run, index);
    if (descriptor == PrCssDescriptor_None)
        return Truth_Unknown;
    CssMediaValue held = featureValue(environment, descriptor);
    if (held.keyword == PrCssKeyword_Unknown)
        return isNanF64(held.number) ? Truth_Unknown : truthOf(held.number != 0);
    return truthOf(held.keyword != PrCssKeyword_None && held.keyword != PrCssKeyword_NoPreference);
}

// Returns the rank of `keyword` among the values of a feature whose values nest, in which a device that matches one
// value matches every lesser one: color-gamut's srgb, p3 and rec2020 (mediaqueries-5/#color-gamut), and dynamic-range's
// standard and high (mediaqueries-5/#dynamic-range). Returns -1 for any other feature or keyword.
int32_t nestedRank(PrCssDescriptor descriptor, PrCssKeyword keyword) {
    switch (descriptor) {
        case PrCssDescriptor_MediaColorGamut:
        case PrCssDescriptor_MediaVideoColorGamut:
            switch (keyword) {
                case PrCssKeyword_Srgb: return 0;
                case PrCssKeyword_P3: return 1;
                case PrCssKeyword_Rec2020: return 2;
                default: return -1;
            }
        case PrCssDescriptor_MediaDynamicRange:
        case PrCssDescriptor_MediaVideoDynamicRange:
            switch (keyword) {
                case PrCssKeyword_Standard: return 0;
                case PrCssKeyword_High: return 1;
                default: return -1;
            }
        default:
            return -1;
    }
}

// Evaluates the <mf-plain> or <mf-range> at `index`: every comparison that it makes of the feature's value with one of
// its <mf-value>s. A plain feature compares for equality. If its name has a min- or max- prefix, it compares for at
// least or at most the value (mediaqueries-5/#mq-min-max).
Truth featureComparison(CssValueRun run, uint32_t index, const CssMediaEnvironment* environment) {
    const CssValueNode* values = run.values;
    uint32_t end = index + values[index].next;
    uint32_t name = index + 1;
    while (!cssValueIsProduction(&values[name], CssValueDef_MfName))
        name += values[name].next;
    PrCssDescriptor descriptor = featureOf(run, name + 1);
    if (descriptor == PrCssDescriptor_None)
        return Truth_Unknown;

    CssValueAuxFlags flags = values[name + 1].aux;
    Order order = Order_Equal;
    if ((flags & CssValueAuxFlag_FeatureMin) != 0)
        order = Order_Greater | Order_Equal;
    else if ((flags & CssValueAuxFlag_FeatureMax) != 0)
        order = Order_Less | Order_Equal;

    CssMediaValue held = featureValue(environment, descriptor);
    CssMediaValue before = {};
    Truth truth = Truth_True;
    for (uint32_t at = index + 1; at < end; at += values[at].next) {
        const CssValueNode* value = &values[at];
        if (value->kind != PrCssValueKind_Production)
            continue;
        CssMediaValue given;
        switch (cssValueProduction(value)) {
            case CssValueDef_MfName:
                if (at != index + 1)
                    truth = both(truth, compare(held, before, flipped(order)));
                break;
            case CssValueDef_MfValue:
                if (!mediaValue(run, at + 1, at + value->next, environment, &given))
                    return Truth_Unknown;
                // A nested value that the device's value includes matches as the device's value.
                if (nestedRank(descriptor, given.keyword) >= 0
                    && nestedRank(descriptor, given.keyword) <= nestedRank(descriptor, held.keyword))
                    given = held;
                if (at < name)
                    before = given;
                else
                    truth = both(truth, compare(held, given, order));
                break;
            default:
                order = comparison(run, at);
                break;
        }
    }
    return truth;
}

// Replaces the truths of the operands of the <media-condition> at `index`, which are the last entries of `stack`, with
// the truth of the condition.
void combine(CssValueRun run, uint32_t index, Array<Truth>* stack) {
    const CssValueNode* values = run.values;
    uint32_t end = index + values[index].next;
    uint32_t operands = 0;
    bool any = false;
    for (uint32_t at = index + 1; at < end; at += values[at].next) {
        ++operands;
        any = any || cssValueIsProduction(&values[at], CssValueDef_MediaOr);
    }
    uint32_t first = stack->count - operands;
    Truth least = Truth_True;
    Truth greatest = Truth_False;
    for (uint32_t i = first; i < stack->count; ++i) {
        least = both(least, stack->data[i]);
        greatest = either(greatest, stack->data[i]);
    }
    stack->count = first;
    stack->push(any ? greatest : least);
}

// Returns true if the <media-type> at `index` matches the environment. `all` and the environment's type match, and
// every other type does not (mediaqueries-5/#media-types).
Truth typeMatches(CssValueRun run, uint32_t index, const CssMediaEnvironment* environment) {
    PrCssKeyword type = cssKeywordFromText(cssValueText(run, &run.values[index + 1]));
    return truthOf(type == PrCssKeyword_All || type == environment->type);
}

// Returns the truth of a <media-query>, whose node is the first node of `query`. The query is read from its end, so
// every <media-in-parens> is evaluated before the condition that contains it, and leaves its truth on `stack`.
Truth evaluateQuery(CssValueRun query, const CssMediaEnvironment* environment,
                    Array<Truth>* stack) {
    const CssValueNode* values = query.values;
    if (query.count == 1)
        return Truth_False;
    stack->count = 0;
    for (uint32_t i = query.count; i-- > 1;) {
        const CssValueNode* value = &values[i];
        if (value->kind != PrCssValueKind_Production)
            continue;
        switch (cssValueProduction(value)) {
            case CssValueDef_MfPlain:
            case CssValueDef_MfRange:
                stack->push(featureComparison(query, i, environment));
                break;
            case CssValueDef_MediaInParens:
                if (cssValueIsLiteral(&values[i + 1], '(')
                    && cssValueIsProduction(&values[i + 2], CssValueDef_MfName))
                    stack->push(booleanFeature(query, i + 3, environment));
                break;
            case CssValueDef_GeneralEnclosed:
                stack->push(Truth_Unknown);
                break;
            case CssValueDef_MediaNot:
                stack->data[stack->count - 1] = negated(stack->data[stack->count - 1]);
                break;
            case CssValueDef_MediaCondition:
            case CssValueDef_MediaConditionWithoutOr:
                combine(query, i, stack);
                break;
            default:
                break;
        }
    }
    if (cssValueIsProduction(&values[1], CssValueDef_MediaCondition))
        return stack->data[0];

    // [ not | only ]? <media-type> [ and <media-condition-without-or> ]?
    uint32_t type = values[1].kind == PrCssValueKind_Keyword ? 2 : 1;
    Truth truth = typeMatches(query, type, environment);
    if (stack->count != 0)
        truth = both(truth, stack->data[0]);
    return cssValueIsKeyword(&values[1], PrCssKeyword_Not) ? negated(truth) : truth;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Environments

void cssMediaEnvironmentInit(CssMediaEnvironment* environment, bool scripting) {
    environment->type = PrCssKeyword_Screen;
    environment->fontSize = 16;
    environment->headroom = 0;
    environment->pixelRatio = 1;
    for (CssMediaValue& value : environment->features)
        value = mediaNumber(kNanF64);
    for (CssMediaValue& value : environment->overrides)
        value = mediaKeyword(PrCssKeyword_Unknown);

    struct MediaNumber {
        PrCssDescriptor descriptor;
        double number;
    };
    constexpr MediaNumber kNumbers[] = {
        {PrCssDescriptor_MediaWidth, 800},
        {PrCssDescriptor_MediaHeight, 600},
        {PrCssDescriptor_MediaDeviceWidth, 800},
        {PrCssDescriptor_MediaDeviceHeight, 600},
        {PrCssDescriptor_MediaResolution, 1},
        {PrCssDescriptor_MediaColor, 8},
        {PrCssDescriptor_MediaColorIndex, 0},
        {PrCssDescriptor_MediaMonochrome, 0},
        {PrCssDescriptor_MediaGrid, 0},
        {PrCssDescriptor_MediaHorizontalViewportSegments, 1},
        {PrCssDescriptor_MediaVerticalViewportSegments, 1},
        {PrCssDescriptor_MediaWebkitTransform3d, 1},
    };
    for (const MediaNumber& entry : kNumbers)
        *valueOf(environment, entry.descriptor) = mediaNumber(entry.number);

    struct MediaKeyword {
        PrCssDescriptor descriptor;
        PrCssKeyword keyword;
    };
    const MediaKeyword kKeywords[] = {
        {PrCssDescriptor_MediaScan, PrCssKeyword_Progressive},
        {PrCssDescriptor_MediaUpdate, PrCssKeyword_Fast},
        {PrCssDescriptor_MediaOverflowBlock, PrCssKeyword_Scroll},
        {PrCssDescriptor_MediaOverflowInline, PrCssKeyword_Scroll},
        {PrCssDescriptor_MediaPointer, PrCssKeyword_Fine},
        {PrCssDescriptor_MediaHover, PrCssKeyword_Hover},
        {PrCssDescriptor_MediaAnyPointer, PrCssKeyword_Fine},
        {PrCssDescriptor_MediaAnyHover, PrCssKeyword_Hover},
        {PrCssDescriptor_MediaColorGamut, PrCssKeyword_Srgb},
        {PrCssDescriptor_MediaVideoColorGamut, PrCssKeyword_Srgb},
        {PrCssDescriptor_MediaDynamicRange, PrCssKeyword_Standard},
        {PrCssDescriptor_MediaVideoDynamicRange, PrCssKeyword_Standard},
        {PrCssDescriptor_MediaEnvironmentBlending, PrCssKeyword_Opaque},
        {PrCssDescriptor_MediaInvertedColors, PrCssKeyword_None},
        {PrCssDescriptor_MediaForcedColors, PrCssKeyword_None},
        {PrCssDescriptor_MediaPrefersColorScheme, PrCssKeyword_Light},
        {PrCssDescriptor_MediaPrefersContrast, PrCssKeyword_NoPreference},
        {PrCssDescriptor_MediaPrefersReducedMotion, PrCssKeyword_NoPreference},
        {PrCssDescriptor_MediaPrefersReducedTransparency, PrCssKeyword_NoPreference},
        {PrCssDescriptor_MediaPrefersReducedData, PrCssKeyword_NoPreference},
        {PrCssDescriptor_MediaScripting,
         PrCssKeyword(scripting ? PrCssKeyword_Enabled : PrCssKeyword_None)},
        {PrCssDescriptor_MediaDisplayMode, PrCssKeyword_Browser},
        {PrCssDescriptor_MediaDisplayState, PrCssKeyword_Normal},
        {PrCssDescriptor_MediaResizable, PrCssKeyword_True},
        {PrCssDescriptor_MediaNavControls, PrCssKeyword_None},
        {PrCssDescriptor_MediaShape, PrCssKeyword_Rect},
        {PrCssDescriptor_MediaUaColorScheme, PrCssKeyword_Light},
    };
    for (const MediaKeyword& entry : kKeywords)
        *valueOf(environment, entry.descriptor) = mediaKeyword(entry.keyword);
}

bool cssMediaEnvironmentSet(CssMediaEnvironment* environment, PrCssDescriptor feature, PrStringView value) {
    PR_ASSERT(feature >= kCssDescriptorMediaFirst && feature < kCssDescriptorMediaEnd
              && !isComputed(feature));

    CssParser parser;
    cssParserInit(&parser);
    CssTokenStream all = cssParserTokenize(&parser, value);
    CssTokenStream ts = cssSubStream(&all, cssStreamTrimmed(&all));
    CssValueSpan matched;
    CssMediaValue held;
    bool set =
        cssParseValue(kCssDescriptorGrammarRoot[feature], &ts, &parser.pool, &parser.match,
                      &matched)
        && mediaValue(cssValuePoolRun(&parser.pool, matched), 0, matched.count, environment, &held);
    if (set)
        *valueOf(environment, feature) = held;
    cssParserReset(&parser);
    return set;
}

void cssMediaEnvironmentOverride(CssMediaEnvironment* environment, PrCssDescriptor feature,
                                 PrCssKeyword keyword) {
    environment->overrides[feature - kCssDescriptorMediaFirst] = mediaKeyword(keyword);
}

PrCssKeyword cssMediaEnvironmentKeyword(const CssMediaEnvironment* environment,
                                        PrCssDescriptor feature) {
    return valueOf(environment, feature)->keyword;
}

float cssMediaEnvironmentPixelRatio(const CssMediaEnvironment* PR_NULLABLE environment) {
    return environment != nullptr ? environment->pixelRatio : 1;
}

float cssMediaEnvironmentHeadroom(const CssMediaEnvironment* PR_NULLABLE environment) {
    return environment != nullptr ? environment->headroom : 0;
}

bool cssMediaEnvironmentPrefersDark(const CssMediaEnvironment* PR_NULLABLE environment) {
    if (environment == nullptr)
        return false;
    constexpr PrCssDescriptor kFeature = PrCssDescriptor_MediaPrefersColorScheme;
    PrCssKeyword preference = environment->overrides[kFeature - kCssDescriptorMediaFirst].keyword;
    if (preference == PrCssKeyword_Unknown)
        preference = valueOf(environment, kFeature)->keyword;
    return preference == PrCssKeyword_Dark;
}

bool cssMediaEnvironmentSetDisplay(CssMediaEnvironment* environment, const PrAppDisplay& display) {
    float stops = display.headroom > 1 ? log2F32(display.headroom) : 0;
    PrCssKeyword range = stops > 0 ? PrCssKeyword_High : PrCssKeyword_Standard;
    PrCssKeyword colors = PrCssKeyword_Srgb;
    if (display.gamut == PrColorSpace_DisplayP3 || display.gamut == PrColorSpace_DisplayP3Linear)
        colors = PrCssKeyword_P3;
    else if (display.gamut == PrColorSpace_Rec2020 || display.gamut == PrColorSpace_Rec2100Linear
             || display.gamut == PrColorSpace_Rec2100Pq || display.gamut == PrColorSpace_Rec2100Hlg)
        colors = PrCssKeyword_Rec2020;
    float ratio = display.pixelRatio > 0 ? display.pixelRatio : 1;
    CssMediaValue* resolution = valueOf(environment, PrCssDescriptor_MediaResolution);
    if (environment->headroom == stops && environment->pixelRatio == ratio && resolution->number == double(ratio)
        && valueOf(environment, PrCssDescriptor_MediaColorGamut)->keyword == colors)
        return false;
    environment->headroom = stops;
    environment->pixelRatio = ratio;
    *resolution = mediaNumber(double(ratio));
    *valueOf(environment, PrCssDescriptor_MediaColorGamut) = mediaKeyword(colors);
    *valueOf(environment, PrCssDescriptor_MediaVideoColorGamut) = mediaKeyword(colors);
    *valueOf(environment, PrCssDescriptor_MediaDynamicRange) = mediaKeyword(range);
    *valueOf(environment, PrCssDescriptor_MediaVideoDynamicRange) = mediaKeyword(range);
    return true;
}

bool cssMediaEnvironmentSetUserPreferences(CssMediaEnvironment* environment, const PrAppUserPreferences& preferences) {
    PrCssKeyword contrast = PrCssKeyword_NoPreference;
    switch (preferences.contrast) {
        case PrAppContrast_More: contrast = PrCssKeyword_More; break;
        case PrAppContrast_Less: contrast = PrCssKeyword_Less; break;
        case PrAppContrast_Custom: contrast = PrCssKeyword_Custom; break;
        default: break;
    }
    struct Preference {
        PrCssDescriptor feature;
        PrCssKeyword keyword;
    };
    const Preference set[] = {
        {PrCssDescriptor_MediaPrefersColorScheme,
         PrCssKeyword(preferences.colorScheme == PrAppColorScheme_Dark ? PrCssKeyword_Dark : PrCssKeyword_Light)},
        {PrCssDescriptor_MediaPrefersContrast, contrast},
        {PrCssDescriptor_MediaPrefersReducedMotion,
         PrCssKeyword(preferences.reducedMotion ? PrCssKeyword_Reduce : PrCssKeyword_NoPreference)},
        {PrCssDescriptor_MediaPrefersReducedTransparency,
         PrCssKeyword(preferences.reducedTransparency ? PrCssKeyword_Reduce : PrCssKeyword_NoPreference)},
    };
    bool changed = false;
    for (const Preference& preference : set) {
        CssMediaValue* value = valueOf(environment, preference.feature);
        if (value->keyword == preference.keyword)
            continue;
        *value = mediaKeyword(preference.keyword);
        changed = true;
    }
    return changed;
}

void cssMediaEnvironmentSetType(CssMediaEnvironment* environment, PrCssMediaType type) {
    environment->type = type == PrCssMediaType_Print ? PrCssKeyword_Print : PrCssKeyword_Screen;
}

// ---------------------------------------------------------------------------------------------------------------------
// Evaluation

bool cssMediaListMatches(const CssMediaQueries* list, const CssMediaEnvironment* environment) {
    CssValueRun run = cssValueListRun(&list->queries);
    bool matches = run.count == 0;
    Array<Truth> stack = {};
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (cssValueIsProduction(&run.values[i], CssValueDef_MediaQuery))
            matches = matches
                      || evaluateQuery(cssValueSubtree(run, i), environment, &stack) == Truth_True;
    }
    stack.reset();
    return matches;
}

const CssMediaEnvironment* PR_NULLABLE cssStyleSheetMedia(const PrCssStyleSheet* sheet) {
    while (sheet->ownerRule != nullptr && sheet->ownerRule->sheet != nullptr)
        sheet = sheet->ownerRule->sheet;
    return sheet->ownerNode != nullptr ? sheet->ownerNode->document->media : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Media query lists

PrCssMediaQueryList* cssMediaQueryListCreate(const CssMediaEnvironment* environment,
                                             PrStringView query) {
    PrCssMediaQueryList* list = allocZeroed<PrCssMediaQueryList>();
    list->type = DomTargetType_MediaQueryList;
    list->destroy = [](DomCountedTarget* target) {
        PrCssMediaQueryList* list = static_cast<PrCssMediaQueryList*>(target);
        domListenersDestroy(list->listeners);
        cssValueListReset(&list->media.queries);
        prMemoryFree(list);
    };
    list->references = 1;
    list->environment = environment;
    cssMediaListParse(&list->media, query);
    return list;
}

void cssMediaQueryListRetain(PrCssMediaQueryList* list) {
    ++list->references;
}

void cssMediaQueryListRelease(PrCssMediaQueryList* PR_NULLABLE list) {
    if (list != nullptr && --list->references == 0)
        list->destroy(list);
}

} // namespace pr_impl
