// Basic shapes: a computed <basic-shape> as layout draws it.

#include "css/src/css_shape.h"

#include "pr/float.h"
#include "pr/path_data.h"

#include "css/src/css_path.h"
#include "css/src/css_style.h"

namespace pr_impl {

namespace {

// The indices of a function's arguments, from a given child up to the next comma or the function's end.
struct Arguments {
    uint32_t items[8];
    uint32_t count;
    uint32_t end;               // the index after the last argument: a comma, or the function's end
};

// Returns the arguments of the function at `index` from its child `from` up to the next comma.
Arguments argumentsOf(CssValueRun run, uint32_t index, uint32_t from) {
    Arguments args{};
    uint32_t end = index + run.values[index].next;
    uint32_t i = from;
    for (; i < end && !cssValueIsLiteral(&run.values[i], ','); i += run.values[i].next) {
        if (args.count < 8)
            args.items[args.count++] = i;
    }
    args.end = i;
    return args;
}

PrCssKeyword keywordAt(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    return value->kind == PrCssValueKind_Keyword ? cssValueKeyword(value) : PrCssKeyword_Count;
}

// Returns the index of the `round` keyword among the function's arguments, or kArrayMissing.
uint32_t roundOf(CssValueRun run, const Arguments& args) {
    for (uint32_t k = 0; k < args.count; ++k) {
        if (keywordAt(run, args.items[k]) == PrCssKeyword_Round)
            return args.items[k];
    }
    return kArrayMissing;
}

PrLayoutRadialExtent extentOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_FarthestSide: return PrLayoutRadialExtent_FarthestSide;
        case PrCssKeyword_ClosestCorner: return PrLayoutRadialExtent_ClosestCorner;
        case PrCssKeyword_FarthestCorner: return PrLayoutRadialExtent_FarthestCorner;
        default: return PrLayoutRadialExtent_ClosestSide;
    }
}

// Writes the radii of `round <'border-radius'>`, read from `from` up to `end`, to `radii` in the order of
// PrLayoutShape::radii (css-backgrounds-3/#border-radius). The value gives one to four horizontal radii and, after a
// slash, one to four vertical radii. The vertical radii default to the horizontal radii.
void cornersOf(CssValueRun run, uint32_t from, uint32_t end, const CssShapeLengths& lengths, PrLayoutLength* radii) {
    uint32_t items[2][4] = {};
    uint32_t counts[2] = {};
    uint32_t axis = 0;
    for (uint32_t i = from; i < end; i += run.values[i].next) {
        if (cssValueIsLiteral(&run.values[i], '/')) {
            axis = 1;
            continue;
        }
        if (counts[axis] < 4)
            items[axis][counts[axis]++] = i;
    }
    if (counts[1] == 0) {
        for (uint32_t c = 0; c < 4; ++c)
            items[1][c] = items[0][c];
        counts[1] = counts[0];
    }
    for (uint32_t a = 0; a < 2; ++a) {
        if (counts[a] == 0)
            continue;
        // Missing values repeat as border-radius repeats them: the third from the first, and the fourth from the
        // second.
        uint32_t* list = items[a];
        if (counts[a] < 2)
            list[1] = list[0];
        if (counts[a] < 3)
            list[2] = list[0];
        if (counts[a] < 4)
            list[3] = list[1];
        for (uint32_t c = 0; c < 4; ++c)
            radii[2 * c + a] = lengths.length(lengths.context, cssValueSubtree(run, list[c]),
                a == 0 ? CssShapeAxis_X : CssShapeAxis_Y);
    }
}

// Converts inset( <length-percentage>{1,4} [ round <'border-radius'> ]? ) (css-shapes-1/#funcdef-basic-shape-inset).
void insetOf(CssValueRun run, uint32_t index, const CssShapeLengths& lengths, PrLayoutShape* out) {
    Arguments args = argumentsOf(run, index, index + 1);
    uint32_t round = roundOf(run, args);
    uint32_t items[4] = {};
    uint32_t count = 0;
    for (uint32_t k = 0; k < args.count && args.items[k] != round && count < 4; ++k)
        items[count++] = args.items[k];
    if (count == 0)
        return;
    if (count < 2)
        items[1] = items[0];
    if (count < 3)
        items[2] = items[0];
    if (count < 4)
        items[3] = items[1];
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        out->inset[side] = lengths.length(lengths.context, cssValueSubtree(run, items[side]),
            side % 2 == 0 ? CssShapeAxis_Y : CssShapeAxis_X);
    if (round != kArrayMissing)
        cornersOf(run, round + 1, args.end, lengths, out->radii);
}

// Converts circle( [ <radial-extent> | <length-percentage> ]? [ at <position> ]? ), or ellipse() with two radii
// (css-shapes-1/#funcdef-basic-shape-circle). An omitted radius is closest-side, and an omitted position the center.
void ellipseOf(CssValueRun run, uint32_t index, bool circle, const CssShapeLengths& lengths, PrLayoutShape* out) {
    Arguments args = argumentsOf(run, index, index + 1);
    uint32_t at = args.count;
    for (uint32_t k = 0; k < args.count; ++k) {
        if (keywordAt(run, args.items[k]) == PrCssKeyword_At) {
            at = k;
            break;
        }
    }
    if (at + 1 < args.count) {
        uint32_t start = args.items[at + 1];
        uint32_t end = args.items[args.count - 1] + run.values[args.items[args.count - 1]].next;
        CssValueRun position{run.values + start, end - start, run.text};
        cssPositionAxes(position, [&](bool vertical, uint32_t value, uint32_t offset) {
            (vertical ? out->centerY : out->centerX) = lengths.position(
                lengths.context, position, value, offset, vertical ? CssShapeAxis_Y : CssShapeAxis_X);
        });
    }
    uint32_t radii = circle ? 1 : 2;
    for (uint32_t axis = 0; axis < radii && axis < at; ++axis) {
        PrCssKeyword keyword = keywordAt(run, args.items[axis]);
        PrLayoutRadialExtent& extent = axis == 0 ? out->extentX : out->extentY;
        if (keyword != PrCssKeyword_Count) {
            extent = extentOf(keyword);
            continue;
        }
        extent = PrLayoutRadialExtent_Radii;
        CssShapeAxis base = circle ? CssShapeAxis_Diagonal : axis == 0 ? CssShapeAxis_X : CssShapeAxis_Y;
        (axis == 0 ? out->radiusX : out->radiusY) =
            lengths.length(lengths.context, cssValueSubtree(run, args.items[axis]), base);
    }
}

// Converts polygon( <'fill-rule'>? [ round <length> ]? , [ <length-percentage> <length-percentage> ]# )
// (css-shapes-1/#funcdef-basic-shape-polygon).
void polygonOf(CssValueRun run, uint32_t index, const CssShapeLengths& lengths, Array<PrLayoutLength>& points,
        PrLayoutShape* out) {
    uint32_t end = index + run.values[index].next;
    uint32_t i = index + 1;
    // The prelude, if the shape has one, before the first comma.
    Arguments prelude = argumentsOf(run, index, i);
    bool hasPrelude = prelude.end < end && prelude.count != 0 && keywordAt(run, prelude.items[0]) != PrCssKeyword_Count;
    for (uint32_t k = 0; hasPrelude && k < prelude.count; ++k) {
        PrCssKeyword keyword = keywordAt(run, prelude.items[k]);
        if (keyword == PrCssKeyword_Evenodd)
            out->rule = PrPathFillRule_EvenOdd;
        if (keyword == PrCssKeyword_Round && k + 1 < prelude.count) {
            double px = 0;
            out->round = cssResolveLength(run, prelude.items[k + 1], 0, &px) ? prF32Max(float(px), 0) : 0;
        }
    }
    if (hasPrelude)
        i = prelude.end + 1;
    while (i < end) {
        Arguments point = argumentsOf(run, index, i);
        if (point.count >= 2) {
            points.push(lengths.length(lengths.context, cssValueSubtree(run, point.items[0]), CssShapeAxis_X));
            points.push(lengths.length(lengths.context, cssValueSubtree(run, point.items[1]), CssShapeAxis_Y));
            ++out->pointCount;
        }
        i = point.end + 1;
    }
}

// Converts path( <'fill-rule'>? , <string> ) (css-shapes-1/#funcdef-basic-shape-path).
void pathOf(CssValueRun run, uint32_t index, PrPath* path, PrLayoutShape* out) {
    if (keywordAt(run, index + 1) == PrCssKeyword_Evenodd)
        out->rule = PrPathFillRule_EvenOdd;
    Array<PrPathDataCommand> commands;
    cssPathCommands(run, index, &commands);
    prPathAppendCommands(path, commands.data, commands.count);
    commands.reset();
}

// The lengths of a basic shape resolved to pixels in a reference box, as a client without a layout tree takes them,
// scaled by `scale`.
struct BoxLengths {
    PrVec2 extent;              // the size of the reference box, unscaled
    float scale;
};

float baseOf(const BoxLengths& b, CssShapeAxis axis) {
    switch (axis) {
        case CssShapeAxis_X: return b.extent.x;
        case CssShapeAxis_Y: return b.extent.y;
        default: return prF32Sqrt((b.extent.x * b.extent.x + b.extent.y * b.extent.y) / 2);
    }
}

PrLayoutLength boxLength(void* PR_NULLABLE context, CssValueRun run, CssShapeAxis axis) {
    const BoxLengths& b = *static_cast<const BoxLengths*>(context);
    if (run.count == 0)
        return prLayoutLengthPx(0);
    if (run.values[0].kind == PrCssValueKind_Percentage)
        return prLayoutLengthPercent(float(run.values[0].number));
    double px = 0;
    return prLayoutLengthPx(cssResolveLength(run, 0, double(baseOf(b, axis)), &px) ? float(px) * b.scale : 0);
}

PrLayoutLength boxPosition(void* PR_NULLABLE context, CssValueRun run, uint32_t index, uint32_t offset, CssShapeAxis axis) {
    const BoxLengths& b = *static_cast<const BoxLengths*>(context);
    float whole = baseOf(b, axis);
    PrCssKeyword keyword = keywordAt(run, index);
    if (keyword == PrCssKeyword_Count)
        return boxLength(context, cssValueSubtree(run, index), axis);
    double by = 0;
    if (offset != kArrayMissing)
        cssResolveLength(run, offset, double(whole), &by);
    if (keyword == PrCssKeyword_Center)
        return prLayoutLengthPercent(50);
    if (keyword == PrCssKeyword_Right || keyword == PrCssKeyword_Bottom)
        return prLayoutLengthPx((whole - float(by)) * b.scale);
    return prLayoutLengthPx(float(by) * b.scale);
}

// The size that a reference box is scaled up to before its lengths resolve, and the most that it is scaled by.
constexpr float kScaledSize = 1024;
constexpr float kMaxScale = float(1 << 20);

} // namespace

PrDrawComposite cssMaskCompositeOperator(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Subtract: return PrDrawComposite_SourceOut;
        case PrCssKeyword_Intersect: return PrDrawComposite_SourceIn;
        case PrCssKeyword_Exclude: return PrDrawComposite_Xor;
        default: return PrDrawComposite_SourceOver;
    }
}

bool cssIsBasicShape(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    if (value->kind != PrCssValueKind_Function)
        return false;
    switch (cssValueKeyword(value)) {
        case PrCssKeyword_Inset:
        case PrCssKeyword_Xywh:
        case PrCssKeyword_Rect:
        case PrCssKeyword_Circle:
        case PrCssKeyword_Ellipse:
        case PrCssKeyword_Polygon:
        case PrCssKeyword_Path:
        case PrCssKeyword_Shape:
            return true;
        default:
            return false;
    }
}

bool cssShapeFromValue(CssValueRun run, uint32_t index, const CssShapeLengths& lengths, Array<PrLayoutLength>& points,
        PrPath* path, PrLayoutShape* out) {
    *out = prLayoutShapeDefault();
    if (!cssIsBasicShape(run, index))
        return false;
    switch (cssValueKeyword(&run.values[index])) {
        case PrCssKeyword_Inset: insetOf(run, index, lengths, out); return true;
        case PrCssKeyword_Circle:
        case PrCssKeyword_Ellipse: {
            bool circle = cssValueKeyword(&run.values[index]) == PrCssKeyword_Circle;
            out->kind = circle ? PrLayoutShapeKind_Circle : PrLayoutShapeKind_Ellipse;
            ellipseOf(run, index, circle, lengths, out);
            return true;
        }
        case PrCssKeyword_Polygon:
            out->kind = PrLayoutShapeKind_Polygon;
            polygonOf(run, index, lengths, points, out);
            return true;
        case PrCssKeyword_Path:
            out->kind = PrLayoutShapeKind_Path;
            pathOf(run, index, path, out);
            return true;
        default: return false;
    }
}

bool cssShapePath(CssValueRun run, uint32_t index, PrRect box, PrPath* out, PrPathFillRule* rule) {
    *rule = PrPathFillRule_NonZero;
    // A layout length holds whole 64ths of a pixel, which is coarse for a small box, such as a box in user units under
    // a viewBox. The shape is therefore resolved in the box scaled up by a power of two, and its outline is scaled back
    // down.
    PrVec2 extent{prRectWidth(box), prRectHeight(box)};
    float size = prF32Max(prF32Abs(extent.x), prF32Abs(extent.y));
    float scale = 1;
    while (size * scale < kScaledSize && scale < kMaxScale)
        scale *= 2;
    PrRect scaled{box.left * scale, box.top * scale, box.right * scale, box.bottom * scale};
    BoxLengths lengths{extent, scale};
    Array<PrLayoutLength> points;
    PrPath* path = prPathCreate();
    PrLayoutShape shape{};
    bool drawn = cssShapeFromValue(run, index, CssShapeLengths{boxLength, boxPosition, &lengths}, points, path, &shape);
    if (drawn) {
        shape.points = points.data;
        prPathTransform(path, prAffine2Scale(PrVec2{scale, scale}));
        shape.path = path;
        shape.round *= scale;
        *rule = shape.rule;
        PrPath* outline = prPathCreate();
        prLayoutShapePath(&shape, nullptr, scaled, outline);
        prPathAddPath(out, outline, prAffine2Scale(PrVec2{1 / scale, 1 / scale}));
        prPathDestroy(outline);
    }
    prPathDestroy(path);
    points.reset();
    return drawn;
}

// ---------------------------------------------------------------------------------------------------------------------
// Corner shapes

bool cssIsCornerShape(PrCssProperty property) {
    return property == PrCssProperty_CornerTopLeftShape || property == PrCssProperty_CornerTopRightShape ||
        property == PrCssProperty_CornerBottomRightShape || property == PrCssProperty_CornerBottomLeftShape;
}

double cssCornerShapeParameter(CssValueRun run) {
    if (run.count == 0)
        return 1;
    if (cssValueIsFunction(run.values, PrCssKeyword_Superellipse) && run.count > 1) {
        const CssValueNode* argument = &run.values[1];
        if (cssValueIsNumeric(argument->kind))
            return argument->number;
        if (cssValueIsKeyword(argument, PrCssKeyword_Infinity))
            return kPrF64Infinity;
        if (cssValueIsKeyword(argument, PrCssKeyword_MinusInfinity))
            return -kPrF64Infinity;
        return 1;
    }
    switch (run.values[0].kind == PrCssValueKind_Keyword ? cssValueKeyword(run.values) : PrCssKeyword_Count) {
        case PrCssKeyword_Scoop: return -1;
        case PrCssKeyword_Bevel: return 0;
        case PrCssKeyword_Notch: return -kPrF64Infinity;
        case PrCssKeyword_Square: return kPrF64Infinity;
        case PrCssKeyword_Squircle: return 2;
        default: return 1;
    }
}

void cssValuePoolAddSuperellipse(CssValuePool* pool, double k) {
    uint32_t function = cssValuePoolAdd(pool, cssFunctionValue(PrCssKeyword_Superellipse));
    if (prF64IsFinite(k))
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, k));
    else
        cssValuePoolAdd(pool, cssKeywordValue(k > 0 ? PrCssKeyword_Infinity : PrCssKeyword_MinusInfinity));
    pool->values.data[function].next = pool->values.count - function;
}

void cssCornerShapeInterpolate(CssValuePool* out, CssValueRun from, CssValueRun to, double progress) {
    // The normalized half corner of K: where a convex corner's curve crosses its diagonal, as a fraction of its radius
    // from the far corner, and the mirror of that for a concave one.
    auto halfCorner = [](double k) {
        if (!prF64IsFinite(k))
            return k > 0 ? 1.0 : 0.0;
        double convex = prF64Pow(0.5, 1 / prF64Pow(2, prF64Abs(k)));
        return k < 0 ? 1 - convex : convex;
    };
    double half = halfCorner(cssCornerShapeParameter(from)) * (1 - progress) + halfCorner(cssCornerShapeParameter(to)) * progress;
    if (half <= 0 || half >= 1) {
        cssValuePoolAddSuperellipse(out, half <= 0 ? -kPrF64Infinity : kPrF64Infinity);
        return;
    }
    double convex = half < 0.5 ? 1 - half : half;
    double k = prF64Log(prF64Log(0.5) / prF64Log(convex)) / prF64Log(2);
    cssValuePoolAddSuperellipse(out, half < 0.5 ? -k : k);
}

} // namespace pr_impl
