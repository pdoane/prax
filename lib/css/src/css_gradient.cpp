// Gradients: a computed <gradient> as layout draws it.

#include "css/src/css_gradient.h"

#include "pr/color_space.h"

#include "css/src/css_math.h"
#include "css/src/css_style.h"
#include "css/src/css_unit.h"

namespace pr_impl {

namespace {

// Returns a computed <angle> in degrees, or 0 if the value is not one.
float degreesOf(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    double degrees = 0;
    if (value->kind == PrCssValueKind_Dimension && cssUnitCategory(value->unit) == CssUnitCategory_Angle)
        degrees = value->number * cssUnitScale(value->unit);
    else if (value->kind == PrCssValueKind_Math)
        cssMathTo(run, index, PrCssUnit_Deg, &degrees);
    return float(degrees);
}

// Returns the hue interpolation method that a keyword names (css-color-4/#hue-interpolation).
PrColorHueMethod hueMethodOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Longer: return PrColorHueMethod_Longer;
        case PrCssKeyword_Increasing: return PrColorHueMethod_Increasing;
        case PrCssKeyword_Decreasing: return PrColorHueMethod_Decreasing;
        default: return PrColorHueMethod_Shorter;
    }
}

// Returns the position of a color stop or hint along a gradient: a length or percentage, or for a conic gradient an
// angle or percentage of the turn, as a percentage.
PrLayoutLength stopPositionOf(const CssLayoutLengths& lengths, CssValueRun run, uint32_t index, bool conic) {
    const CssValueNode* value = &run.values[index];
    if (!conic || value->kind == PrCssValueKind_Percentage)
        return lengths.length(lengths.context, cssValueSubtree(run, index));
    return prLayoutLengthPercent(degreesOf(run, index) / 3.6f);
}

// Returns `color` with each component that it misses taken from `other`, if `other` has it.
CssColor filledFrom(const CssColor& color, const CssColor& other) {
    CssColor out = color;
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t bit = 1u << i;
        if ((color.missing & bit) == 0 || (other.missing & bit) != 0)
            continue;
        out.missing &= ~bit;
        (i < 3 ? out.c[i] : out.alpha) = i < 3 ? other.c[i] : other.alpha;
    }
    return out;
}

// Appends the color stops and hints of a gradient's list, the values from `first` to `end`, to `out`
// (css-images-4/#color-stop-syntax). A color stop with two positions is two stops of its color. Colors convert to the
// gradient's interpolation space `space`, where a missing component takes the value of the color stop that it mixes
// with (css-color-4/#interpolation-missing). A stop that mixes with two stops that differ there becomes two stops at
// its position, one for each side.
void gradientStopsOf(const CssLayoutLengths& lengths, CssValueRun run, uint32_t first, uint32_t end, bool conic,
        PrColorSpace space, const CssColorContext& colors, float cap, Array<PrLayoutColorStop>& out) {
    struct Stop {
        PrLayoutColorStop stop;
        CssColor color;
    };
    Array<Stop> stops;
    uint32_t i = first;
    while (i < end) {
        uint32_t itemEnd = i;
        while (itemEnd < end && !cssValueIsLiteral(&run.values[itemEnd], ','))
            itemEnd += run.values[itemEnd].next;
        const CssValueNode* head = &run.values[i];
        bool hint = head->kind == PrCssValueKind_Percentage || head->kind == PrCssValueKind_Math ||
            (head->kind == PrCssValueKind_Dimension && head->unit != PrCssUnit_Unknown);
        if (hint) {
            stops.push(Stop{
                .stop = {.position = stopPositionOf(lengths, run, i, conic), .kind = PrLayoutColorStopKind_Hint},
            });
        } else {
            Stop stop{.stop = {.position = prLayoutLengthAuto()}};
            if (cssResolveColorComponents(run, i, colors, &stop.color))
                stop.color = cssColorConvert(stop.color, space);
            else
                stop.color = CssColor{PrColorSpace_Srgb, {0, 0, 0}, 0, 0};
            uint32_t at = i + head->next;
            if (at < itemEnd)
                stop.stop.position = stopPositionOf(lengths, run, at, conic);
            stops.push(stop);
            at = at < itemEnd ? at + run.values[at].next : at;
            if (at < itemEnd) {
                stop.stop.position = stopPositionOf(lengths, run, at, conic);
                stops.push(stop);
            }
        }
        i = itemEnd < end ? itemEnd + 1 : end;
    }
    // In a polar space, a stop keeps the hue that its color states, even a powerless one, because its sRGB color cannot
    // carry that hue. lib/draw gives a hue that is still missing the hue of the stop that it mixes with.
    uint32_t hue = prColorSpaceHueChannel(space);
    auto emit = [&](PrLayoutColorStop stop, const CssColor& color) {
        PrColor srgb = cssColorToSrgb(color);
        stop.color = prColor16FromColor(cap > 0 ? prColorLimit(srgb, cap) : srgb);
        if (hue < 3 && (color.missing & (1u << hue)) == 0) {
            stop.hue = float(color.c[hue]);
            stop.hasHue = true;
        }
        out.push(stop);
    };
    for (uint32_t k = 0; k < stops.count; ++k) {
        const Stop& s = stops.data[k];
        if (s.stop.kind == PrLayoutColorStopKind_Hint) {
            out.push(s.stop);
            continue;
        }
        if (s.color.missing == 0) {
            emit(s.stop, s.color);
            continue;
        }
        const CssColor* before = nullptr;
        const CssColor* after = nullptr;
        for (uint32_t j = k; j-- > 0 && before == nullptr;) {
            if (stops.data[j].stop.kind == PrLayoutColorStopKind_Color)
                before = &stops.data[j].color;
        }
        for (uint32_t j = k + 1; j < stops.count && after == nullptr; ++j) {
            if (stops.data[j].stop.kind == PrLayoutColorStopKind_Color)
                after = &stops.data[j].color;
        }
        CssColor left = filledFrom(s.color, before != nullptr ? *before : after != nullptr ? *after : s.color);
        CssColor right = filledFrom(s.color, after != nullptr ? *after : before != nullptr ? *before : s.color);
        emit(s.stop, left);
        bool differs = left.missing != right.missing || left.alpha != right.alpha || left.c[0] != right.c[0] ||
            left.c[1] != right.c[1] || left.c[2] != right.c[2];
        if (differs)
            emit(PrLayoutColorStop{.position = prLayoutLengthNone()}, right);
    }
    stops.reset();
}

// Returns true if a gradient's first value starts its prelude rather than its first color stop: one of the prelude's
// keywords, or a number, which no color starts with.
bool startsPrelude(const CssValueNode* value) {
    if (value->kind != PrCssValueKind_Keyword)
        return value->kind == PrCssValueKind_Dimension || value->kind == PrCssValueKind_Math ||
            value->kind == PrCssValueKind_Percentage || value->kind == PrCssValueKind_Number ||
            value->kind == PrCssValueKind_Integer;
    switch (cssValueKeyword(value)) {
        case PrCssKeyword_To:
        case PrCssKeyword_From:
        case PrCssKeyword_At:
        case PrCssKeyword_In:
        case PrCssKeyword_Circle:
        case PrCssKeyword_Ellipse:
        case PrCssKeyword_ClosestSide:
        case PrCssKeyword_ClosestCorner:
        case PrCssKeyword_FarthestSide:
        case PrCssKeyword_FarthestCorner:
            return true;
        default:
            return false;
    }
}

} // namespace

void cssGradientFromValue(CssValueRun run, uint32_t index, const CssColorContext& colors, float cap,
        const CssLayoutLengths& lengths, Array<PrLayoutColorStop>& stops, PrLayoutGradient* out) {
    PrLayoutGradient g = prLayoutGradientDefault();
    PrCssKeyword function = cssValueKeyword(&run.values[index]);
    switch (function) {
        case PrCssKeyword_RepeatingLinearGradient:
            g.repeating = true;
            break;
        case PrCssKeyword_RadialGradient:
        case PrCssKeyword_RepeatingRadialGradient:
            g.kind = PrLayoutGradientKind_Radial;
            g.repeating = function == PrCssKeyword_RepeatingRadialGradient;
            break;
        case PrCssKeyword_ConicGradient:
        case PrCssKeyword_RepeatingConicGradient:
            g.kind = PrLayoutGradientKind_Conic;
            g.repeating = function == PrCssKeyword_RepeatingConicGradient;
            g.angle = 0;
            break;
        default:
            break;
    }
    uint32_t end = index + run.values[index].next;
    uint32_t i = index + 1;
    // The prelude, up to the first comma: a direction, a shape and size, a position, and an interpolation method. A
    // gradient without one starts with its first color stop, which starts with a color.
    bool interpolates = false;
    uint32_t radii = 0;
    if (i < end && !startsPrelude(&run.values[i]))
        --i;
    while (i < end && i > index && !cssValueIsLiteral(&run.values[i], ',')) {
        const CssValueNode* value = &run.values[i];
        PrCssKeyword keyword = value->kind == PrCssValueKind_Keyword ? cssValueKeyword(value) : PrCssKeyword_Count;
        if (keyword == PrCssKeyword_To) {
            // `to` and one or two sides. The computed value writes the horizontal side first.
            bool left = false, right = false, top = false, bottom = false;
            for (i += 1; i < end && run.values[i].kind == PrCssValueKind_Keyword; ++i) {
                PrCssKeyword side = cssValueKeyword(&run.values[i]);
                if (side != PrCssKeyword_Left && side != PrCssKeyword_Right && side != PrCssKeyword_Top &&
                        side != PrCssKeyword_Bottom)
                    break;
                left = left || side == PrCssKeyword_Left;
                right = right || side == PrCssKeyword_Right;
                top = top || side == PrCssKeyword_Top;
                bottom = bottom || side == PrCssKeyword_Bottom;
            }
            if ((left || right) && top)
                g.corner = left ? PrLayoutGradientCorner_TopLeft : PrLayoutGradientCorner_TopRight;
            else if ((left || right) && bottom)
                g.corner = left ? PrLayoutGradientCorner_BottomLeft : PrLayoutGradientCorner_BottomRight;
            else
                g.angle = top ? 0 : right ? 90 : bottom ? 180 : 270;
            continue;
        }
        if (keyword == PrCssKeyword_From) {
            g.angle = degreesOf(run, i + 1);
            i += 1 + run.values[i + 1].next;
            continue;
        }
        if (keyword == PrCssKeyword_At) {
            uint32_t from = i + 1;
            uint32_t to = from;
            while (to < end && !cssValueIsLiteral(&run.values[to], ',') &&
                    !cssValueIsKeyword(&run.values[to], PrCssKeyword_In))
                to += run.values[to].next;
            CssValueRun position{run.values + from, to - from, run.text};
            cssPositionAxes(position, [&](bool vertical, uint32_t at, uint32_t offset) {
                *(vertical ? &g.centerY : &g.centerX) = lengths.position(lengths.context, position, at, offset);
            });
            i = to;
            continue;
        }
        if (keyword == PrCssKeyword_In) {
            interpolates = true;
            const CssValueNode* named = &run.values[i + 1];
            PrColorSpace space = PrColorSpace_Count;
            if (named->kind == PrCssValueKind_Keyword)
                space = cssColorSpaceFromKeyword(cssValueKeyword(named));
            // A custom color space parses but is not supported, so the gradient interpolates in Oklab.
            g.space = space != PrColorSpace_Count ? space : PrColorSpace_Oklab;
            i += 2;
            if (i + 1 < end && cssValueIsKeyword(&run.values[i + 1], PrCssKeyword_Hue)) {
                g.hueMethod = hueMethodOf(cssValueKeyword(&run.values[i]));
                i += 2;
            }
            continue;
        }
        switch (keyword) {
            case PrCssKeyword_Circle: g.shape = PrLayoutRadialShape_Circle; break;
            case PrCssKeyword_Ellipse: g.shape = PrLayoutRadialShape_Ellipse; break;
            case PrCssKeyword_ClosestSide: g.extent = PrLayoutRadialExtent_ClosestSide; break;
            case PrCssKeyword_ClosestCorner: g.extent = PrLayoutRadialExtent_ClosestCorner; break;
            case PrCssKeyword_FarthestSide: g.extent = PrLayoutRadialExtent_FarthestSide; break;
            case PrCssKeyword_FarthestCorner: g.extent = PrLayoutRadialExtent_FarthestCorner; break;
            default:
                if (g.kind == PrLayoutGradientKind_Linear) {
                    g.angle = degreesOf(run, i);
                } else if (g.kind == PrLayoutGradientKind_Radial) {
                    // One length is a circle's radius, and two are an ellipse's.
                    g.extent = PrLayoutRadialExtent_Radii;
                    (radii++ == 0 ? g.radiusX : g.radiusY) = lengths.length(lengths.context, cssValueSubtree(run, i));
                }
                break;
        }
        i += value->next;
    }
    if (g.kind == PrLayoutGradientKind_Radial && g.extent == PrLayoutRadialExtent_Radii && radii == 1)
        g.shape = PrLayoutRadialShape_Circle;
    uint32_t first = i == index ? index + 1 : i < end ? i + 1 : end;
    if (!interpolates)
        g.space = cssColorsAreLegacy(run.values, first, end) ? PrColorSpace_Srgb : PrColorSpace_Oklab;
    uint32_t count = stops.count;
    gradientStopsOf(lengths, run, first, end, g.kind == PrLayoutGradientKind_Conic, g.space, colors, cap, stops);
    g.stopCount = stops.count - count;
    *out = g;
}

bool cssIsGradient(CssValueRun run) {
    if (run.count == 0 || run.values[0].kind != PrCssValueKind_Function)
        return false;
    switch (cssValueKeyword(run.values)) {
        case PrCssKeyword_LinearGradient:
        case PrCssKeyword_RepeatingLinearGradient:
        case PrCssKeyword_RadialGradient:
        case PrCssKeyword_RepeatingRadialGradient:
        case PrCssKeyword_ConicGradient:
        case PrCssKeyword_RepeatingConicGradient:
            return true;
        default:
            return false;
    }
}

} // namespace pr_impl
