// SVG styles: the computed values that the SVG tree reads from the cascade.

#include "svg/src/svg_style.h"

#include "pr/float.h"

#include "css/src/css_animation.h"
#include "css/src/css_color.h"
#include "css/src/css_math.h"
#include "css/src/css_path.h"

namespace pr_impl {

namespace {

// Returns the length of the value at `index` of `run`, a computed <length-percentage> or a number in px.
SvgLengthPercentage lengthAt(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer)
        return SvgLengthPercentage{float(value->number), 0};
    if (value->kind == PrCssValueKind_Percentage)
        return SvgLengthPercentage{0, float(value->number)};
    // A length or a calculation is linear in the base of its percentages, so two resolutions give its form.
    double none = 0;
    double whole = 0;
    if (!cssResolveLength(run, index, 0, &none) || !cssResolveLength(run, index, 100, &whole))
        return SvgLengthPercentage{};
    return SvgLengthPercentage{float(none), float(whole - none)};
}

PrPathJoin joinOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Round: return PrPathJoin_Round;
        case PrCssKeyword_Bevel: return PrPathJoin_Bevel;
        default: return PrPathJoin_Miter;
    }
}

PrPathCap capOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Round: return PrPathCap_Round;
        case PrCssKeyword_Square: return PrPathCap_Square;
        default: return PrPathCap_Butt;
    }
}

// Returns the offset of one axis of a computed transform-origin: a length-percentage, or an edge keyword or center as
// the percentage it stands for.
SvgLengthPercentage originAt(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    if (value->kind != PrCssValueKind_Keyword)
        return lengthAt(run, index);
    switch (cssValueKeyword(value)) {
        case PrCssKeyword_Left:
        case PrCssKeyword_Top: return SvgLengthPercentage{0, 0};
        case PrCssKeyword_Right:
        case PrCssKeyword_Bottom: return SvgLengthPercentage{0, 100};
        default: return SvgLengthPercentage{0, 50};
    }
}

// Returns the URL that the value at `index` of `run` names, or the null view.
PrStringView urlAt(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Url)
        return cssValueText(run, value);
    bool function = cssValueIsFunction(value, PrCssKeyword_Url) || cssValueIsFunction(value, PrCssKeyword_Src);
    if (function && value->next >= 2 && value[1].kind == PrCssValueKind_String)
        return cssValueText(run, &value[1]);
    return PrStringView{};
}

} // namespace

SvgLengthPercentage svgStyleLength(const CssComputedStyle* style, PrCssProperty property) {
    CssValueRun run = cssComputedValue(style, property);
    if (run.count == 0 || run.values[0].kind == PrCssValueKind_Keyword)
        return SvgLengthPercentage{};
    return lengthAt(run, 0);
}

SvgVectorEffectFlags svgStyleVectorEffects(const CssComputedStyle* style) {
    CssValueRun run = cssComputedValue(style, PrCssProperty_VectorEffect);
    SvgVectorEffectFlags flags = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (run.values[i].kind != PrCssValueKind_Keyword)
            continue;
        switch (cssValueKeyword(&run.values[i])) {
            case PrCssKeyword_NonScalingSize: flags |= SvgVectorEffectFlag_NonScalingSize; break;
            case PrCssKeyword_NonRotation: flags |= SvgVectorEffectFlag_NonRotation; break;
            case PrCssKeyword_FixedPosition: flags |= SvgVectorEffectFlag_FixedPosition; break;
            default: break;
        }
    }
    return flags;
}

bool svgStyleNonScalingStroke(const CssComputedStyle* style) {
    CssValueRun run = cssComputedValue(style, PrCssProperty_VectorEffect);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (cssValueIsKeyword(&run.values[i], PrCssKeyword_NonScalingStroke))
            return true;
    }
    return false;
}

bool svgStyleIs(const CssComputedStyle* style, PrCssProperty property, PrCssKeyword keyword) {
    return svgStyleKeyword(style, property) == keyword;
}

PrCssKeyword svgStyleKeyword(const CssComputedStyle* style, PrCssProperty property) {
    CssValueRun run = cssComputedValue(style, property);
    if (run.count != 0 && run.values[0].kind == PrCssValueKind_Keyword)
        return cssValueKeyword(run.values);
    return PrCssKeyword_Count;
}

float svgStyleNumber(const CssComputedStyle* style, PrCssProperty property, float fallback) {
    CssValueRun run = cssComputedValue(style, property);
    if (run.count == 0)
        return fallback;
    const CssValueNode* value = run.values;
    if (value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer)
        return float(value->number);
    double number = 0;
    return value->kind == PrCssValueKind_Math && cssMathTo(run, 0, PrCssUnit_Number, &number) ? float(number) : fallback;
}

PrColor svgStyleColor(const CssComputedStyle* style, PrCssProperty property, const CssColorEnvironment& environment) {
    PrColor color{0, 0, 0, 1};
    cssResolveColor(cssComputedValue(style, property), 0, cssStyleColorContext(style, environment), &color);
    return color;
}

SvgPaintStyle svgStylePaint(const CssComputedStyle* style, PrCssProperty property,
        const CssColorEnvironment& environment) {
    CssValueRun run = cssComputedValue(style, property);
    SvgPaintStyle paint{};
    if (run.count == 0)
        return paint;
    PrCssKeyword keyword = PrCssKeyword_Count;
    if (run.values[0].kind == PrCssValueKind_Keyword)
        keyword = cssValueKeyword(run.values);
    if (keyword == PrCssKeyword_None)
        return paint;
    if (keyword == PrCssKeyword_ContextFill || keyword == PrCssKeyword_ContextStroke) {
        paint.kind = keyword == PrCssKeyword_ContextFill ? SvgPaintKind_ContextFill : SvgPaintKind_ContextStroke;
        return paint;
    }
    CssColorContext colors = cssStyleColorContext(style, environment);
    PrStringView url = urlAt(run, 0);
    if (url.data == nullptr) {
        paint.kind = cssResolveColor(run, 0, colors, &paint.color) ? SvgPaintKind_Color : SvgPaintKind_None;
        return paint;
    }
    paint.kind = SvgPaintKind_Url;
    paint.url = url;
    // A server that cannot paint falls back to none if the value names no fallback
    // (SVG2/painting.html#SpecifyingPaint).
    uint32_t next = run.values[0].next;
    if (next < run.count && !cssValueIsKeyword(&run.values[next], PrCssKeyword_None))
        paint.fallback = cssResolveColor(run, next, colors, &paint.color);
    return paint;
}

void svgStylePaintOrder(const CssComputedStyle* style, SvgLayer* order) {
    // The computed value lists the first layers, and the rest follow in the normal order.
    CssValueRun run = cssComputedValue(style, PrCssProperty_PaintOrder);
    uint32_t count = 0;
    bool given[3] = {};
    for (uint32_t i = 0; i < run.count && count < 3; i += run.values[i].next) {
        SvgLayer layer = SvgLayer_Fill;
        if (cssValueIsKeyword(&run.values[i], PrCssKeyword_Stroke))
            layer = SvgLayer_Stroke;
        else if (cssValueIsKeyword(&run.values[i], PrCssKeyword_Markers))
            layer = SvgLayer_Markers;
        else if (!cssValueIsKeyword(&run.values[i], PrCssKeyword_Fill))
            continue;
        if (given[layer])
            continue;
        given[layer] = true;
        order[count++] = layer;
    }
    for (SvgLayer layer = SvgLayer_Fill; layer <= SvgLayer_Markers; ++layer) {
        if (!given[layer])
            order[count++] = layer;
    }
}

PrStringView svgStyleUrl(const CssComputedStyle* style, PrCssProperty property) {
    CssValueRun run = cssComputedValue(style, property);
    return run.count != 0 ? urlAt(run, 0) : PrStringView{};
}

PrStringView svgRunUrl(CssValueRun run) {
    return run.count != 0 ? urlAt(run, 0) : PrStringView{};
}

void svgStylePathCommands(const CssComputedStyle* style, Array<PrPathDataCommand>* out) {
    CssValueRun d = cssComputedValue(style, PrCssProperty_D);
    if (d.count != 0 && d.values[0].kind == PrCssValueKind_Function)
        cssPathCommands(d, 0, out);
}

SvgStrokeStyle svgStyleStroke(const CssComputedStyle* style) {
    SvgStrokeStyle stroke{};
    stroke.width = svgStyleLength(style, PrCssProperty_StrokeWidth);
    stroke.dashOffset = svgStyleLength(style, PrCssProperty_StrokeDashoffset);
    // A miter limit below one is an error that strokes as one (SVG2/painting.html#StrokeMiterlimitProperty).
    stroke.miterLimit = maxF32(svgStyleNumber(style, PrCssProperty_StrokeMiterlimit, 4), 1);
    stroke.join = joinOf(svgStyleKeyword(style, PrCssProperty_StrokeLinejoin));
    stroke.cap = capOf(svgStyleKeyword(style, PrCssProperty_StrokeLinecap));
    CssValueRun dashes = cssComputedValue(style, PrCssProperty_StrokeDasharray);
    for (uint32_t i = 0; i < dashes.count; i += dashes.values[i].next) {
        const CssValueNode* value = &dashes.values[i];
        if (value->kind == PrCssValueKind_Literal || value->kind == PrCssValueKind_Keyword)
            continue;
        if (stroke.dashCount == kSvgMaxDashes) {
            stroke.dashCount = 0;
            break;
        }
        stroke.dashes[stroke.dashCount++] = lengthAt(dashes, i);
    }
    return stroke;
}

SvgLineFlags svgStyleLines(const CssComputedStyle* style) {
    CssValueRun run = cssComputedValue(style, PrCssProperty_TextDecorationLine);
    SvgLineFlags lines = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (run.values[i].kind != PrCssValueKind_Keyword)
            continue;
        PrCssKeyword keyword = cssValueKeyword(&run.values[i]);
        if (keyword == PrCssKeyword_Underline)
            lines |= SvgLineFlag_Underline;
        else if (keyword == PrCssKeyword_Overline)
            lines |= SvgLineFlag_Overline;
        else if (keyword == PrCssKeyword_LineThrough)
            lines |= SvgLineFlag_LineThrough;
    }
    return lines;
}

PrPathFillRule svgStyleFillRule(const CssComputedStyle* style, PrCssProperty property) {
    return svgStyleIs(style, property, PrCssKeyword_Evenodd) ? PrPathFillRule_EvenOdd : PrPathFillRule_NonZero;
}

SvgPointerEvents svgStylePointerEvents(const CssComputedStyle* style) {
    switch (svgStyleKeyword(style, PrCssProperty_PointerEvents)) {
        case PrCssKeyword_None: return SvgPointerEvents_None;
        case PrCssKeyword_BoundingBox: return SvgPointerEvents_BoundingBox;
        case PrCssKeyword_Visiblefill: return SvgPointerEvents_VisibleFill;
        case PrCssKeyword_Visiblestroke: return SvgPointerEvents_VisibleStroke;
        case PrCssKeyword_Visible: return SvgPointerEvents_Visible;
        case PrCssKeyword_Painted: return SvgPointerEvents_Painted;
        case PrCssKeyword_Fill: return SvgPointerEvents_Fill;
        case PrCssKeyword_Stroke: return SvgPointerEvents_Stroke;
        case PrCssKeyword_All: return SvgPointerEvents_All;
        default: return SvgPointerEvents_VisiblePainted;
    }
}

SvgTransformStyle svgStyleTransform(const CssComputedStyle* style) {
    SvgTransformStyle transform{};
    CssValueRun list = cssComputedValue(style, PrCssProperty_Transform);
    CssValueRun translate = cssComputedValue(style, PrCssProperty_Translate);
    CssValueRun rotate = cssComputedValue(style, PrCssProperty_Rotate);
    CssValueRun scale = cssComputedValue(style, PrCssProperty_Scale);
    transform.list = cssTransformFunctions(translate, rotate, scale, list);
    transform.any = transform.list.count != 0;
    if (!transform.any)
        return transform;
    transform.box = svgStyleKeyword(style, PrCssProperty_TransformBox);
    CssValueRun origin = cssComputedValue(style, PrCssProperty_TransformOrigin);
    transform.origin[0] = SvgLengthPercentage{0, 50};
    transform.origin[1] = SvgLengthPercentage{0, 50};
    if (origin.count == 0)
        return transform;
    transform.origin[0] = originAt(origin, 0);
    uint32_t second = origin.values[0].next;
    // A single vertical keyword sets the y of the origin, and leaves x at the center.
    bool vertical = cssValueIsKeyword(origin.values, PrCssKeyword_Top) ||
        cssValueIsKeyword(origin.values, PrCssKeyword_Bottom);
    if (second >= origin.count && vertical) {
        transform.origin[1] = transform.origin[0];
        transform.origin[0] = SvgLengthPercentage{0, 50};
    } else if (second < origin.count) {
        transform.origin[1] = originAt(origin, second);
    }
    return transform;
}

bool svgStyleTransforms3d(const CssComputedStyle* style) {
    CssValueRun list = cssComputedValue(style, PrCssProperty_Transform);
    for (uint32_t i = 0; i < list.count; i += list.values[i].next) {
        if (list.values[i].kind != PrCssValueKind_Function)
            continue;
        switch (cssValueKeyword(&list.values[i])) {
            case PrCssKeyword_Matrix3d:
            case PrCssKeyword_Translate3d:
            case PrCssKeyword_Translatez:
            case PrCssKeyword_Scale3d:
            case PrCssKeyword_Scalez:
            case PrCssKeyword_Rotate3d:
            case PrCssKeyword_Rotatex:
            case PrCssKeyword_Rotatey:
            case PrCssKeyword_Rotatez:
            case PrCssKeyword_Perspective:
                return true;
            default:
                break;
        }
    }
    return false;
}

PrAffine2 svgTransformMatrix(const SvgTransformStyle& transform, PrRect box) {
    if (!transform.any)
        return prAffine2Identity();
    PrVec2 extent{prRectWidth(box), prRectHeight(box)};
    PrMatrix4 matrix = prLayoutTransformListMatrix(&transform.list, extent);
    PrAffine2 functions = prAffine2Identity();
    if (!prAffine2FromMatrix4(&matrix, &functions))
        functions = PrAffine2{
            matrix.m[0][0], matrix.m[1][0], matrix.m[0][1], matrix.m[1][1], matrix.m[0][3],
            matrix.m[1][3],
        };
    PrVec2 origin{
        box.left + svgLengthResolve(transform.origin[0], extent.x),
        box.top + svgLengthResolve(transform.origin[1], extent.y),
    };
    return prAffine2Concat(prAffine2Concat(prAffine2Translate(-origin), functions), prAffine2Translate(origin));
}

void svgStylePosition(const CssComputedStyle* style, PrCssProperty property, SvgLengthPercentage* out) {
    out[0] = SvgLengthPercentage{0, 50};
    out[1] = SvgLengthPercentage{0, 50};
    CssValueRun run = cssComputedValue(style, property);
    cssPositionAxes(run, [&](bool vertical, uint32_t index, uint32_t offset) {
        const CssValueNode* value = &run.values[index];
        SvgLengthPercentage length{};
        if (value->kind != PrCssValueKind_Keyword) {
            length = lengthAt(run, index);
        } else {
            PrCssKeyword keyword = cssValueKeyword(value);
            SvgLengthPercentage by = offset != kArrayMissing ? lengthAt(run, offset) : SvgLengthPercentage{};
            if (keyword == PrCssKeyword_Center)
                length = SvgLengthPercentage{0, 50};
            else if (keyword == PrCssKeyword_Right || keyword == PrCssKeyword_Bottom)
                length = SvgLengthPercentage{-by.px, 100 - by.percent};
            else
                length = by;
        }
        out[vertical ? 1 : 0] = length;
    });
}

} // namespace pr_impl
