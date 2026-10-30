// Backgrounds: the background layers that a tree interns for its decoration groups, and how they paint.
//
// css-backgrounds-3 - https://drafts.csswg.org/css-backgrounds-3/
// css-images-3 - https://drafts.csswg.org/css-images-3/
// css-images-4 - https://drafts.csswg.org/css-images-4/

#include "layout/src/layout_background.h"

#include "pr/draw_filter.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"

#include "layout/src/layout_effects.h"

namespace pr_impl {

namespace {

// The seed for hashing a list's key.
constexpr uint64_t kBackgroundSeed = 0x3c6ef372fe94f82bull;

// The most color stops that a repeating conic gradient expands to. A gradient whose period needs more paints its stops'
// average color, which is what its repetitions converge to.
constexpr uint32_t kMaxRepeatedStops = 4096;

template <typename T>
void appendKey(Array<uint8_t>& key, const T& value) {
    key.append(reinterpret_cast<const uint8_t*>(&value), uint32_t(sizeof(T)));
}

// Appends the bytes that identify `layers`: every field that painting reads, a gradient's stops in place of the pointer
// to them, an image's or list's address, and a painter's function and context.
void backgroundKey(Array<uint8_t>& key, const PrLayoutBackgroundLayer* layers, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrLayoutBackgroundLayer& l = layers[i];
        uint8_t bytes[] = {
            l.kind,   l.rendering, l.repeatX, l.repeatY,   l.attachment,
            l.origin, l.clip,      l.size,    l.blendMode, l.composite,
        };
        appendKey(key, bytes);
        uint32_t words[] = {l.sizeX.bits, l.sizeY.bits, l.positionX.bits, l.positionY.bits};
        appendKey(key, words);
        appendKey(key, l.filterCount);
        layoutFilterKey(key, l.filters, l.filterCount);
        switch (l.kind) {
            case PrLayoutImageKind_Image:
            case PrLayoutImageKind_List:
            case PrLayoutImageKind_Painter: {
                int32_t natural[] = {l.natural.width.raw, l.natural.height.raw};
                appendKey(key, natural);
                appendKey(key, l.natural.ratio);
                appendKey(key, l.limit);
                const void* address = l.painter.context;
                if (l.kind == PrLayoutImageKind_Image)
                    address = l.image;
                else if (l.kind == PrLayoutImageKind_List)
                    address = l.list;
                appendKey(key, address);
                if (l.kind == PrLayoutImageKind_Painter)
                    appendKey(key, l.painter.paint);
                break;
            }
            case PrLayoutImageKind_Gradient: {
                const PrLayoutGradient& g = l.gradient;
                uint8_t fields[] = {g.kind, g.space, g.hueMethod, uint8_t(g.repeating), g.corner, g.shape, g.extent};
                appendKey(key, fields);
                appendKey(key, g.angle);
                uint32_t lengths[] = {g.radiusX.bits, g.radiusY.bits, g.centerX.bits, g.centerY.bits};
                appendKey(key, lengths);
                appendKey(key, g.stopCount);
                for (uint32_t s = 0; s < g.stopCount; ++s)
                    appendKey(key, g.stops[s]);
                break;
            }
            default:
                break;
        }
    }
}

// Adds `delta` to the references of what the layers name: the compounds of their lengths, their images and lists, and
// the contexts of their painters that share them.
void countLayers(LayoutCompounds* compounds, const PrLayoutBackgroundLayer* layers, uint32_t count, uint32_t delta) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrLayoutBackgroundLayer& l = layers[i];
        const PrLayoutLength lengths[] = {l.sizeX, l.sizeY, l.positionX, l.positionY};
        for (PrLayoutLength length : lengths)
            layoutCompoundsCount(compounds, length.bits, delta);
        for (uint32_t f = 0; f < l.filterCount; ++f)
            layoutSourceCount(l.filters[f].source, delta);
        if (l.kind == PrLayoutImageKind_Image) {
            if (delta == 1)
                prDrawImageRetain(l.image);
            else
                prDrawImageRelease(l.image);
        } else if (l.kind == PrLayoutImageKind_List) {
            if (delta == 1)
                prDrawListRetain(l.list);
            else
                prDrawListRelease(l.list);
        } else if (l.kind == PrLayoutImageKind_Painter) {
            if (delta == 1)
                prLayoutPainterRetain(l.painter);
            else
                prLayoutPainterRelease(l.painter);
        } else if (l.kind == PrLayoutImageKind_Gradient) {
            const PrLayoutGradient& g = l.gradient;
            const PrLayoutLength geometry[] = {g.radiusX, g.radiusY, g.centerX, g.centerY};
            for (PrLayoutLength length : geometry)
                layoutCompoundsCount(compounds, length.bits, delta);
            for (uint32_t s = 0; s < g.stopCount; ++s)
                layoutCompoundsCount(compounds, g.stops[s].position.bits, delta);
        }
    }
}

void freeEntry(LayoutBackground& entry) {
    prMemoryFree(entry.layers);
    prMemoryFree(entry.stops);
    prMemoryFree(entry.filters);
    entry.key.reset();
    entry = LayoutBackground{};
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting

float resolvePx(const LayoutCompounds* compounds, PrLayoutLength length, float base) {
    return prLayoutUnitToPx(layoutLengthResolve(compounds, length, prLayoutUnitFromPx(base)));
}

bool hasWidth(const PrLayoutNaturalSize& n) {
    return n.width.raw >= 0;
}
bool hasHeight(const PrLayoutNaturalSize& n) {
    return n.height.raw >= 0;
}

// Returns the natural ratio, width over height, or zero for none.
float ratioOf(const PrLayoutNaturalSize& n) {
    if (hasWidth(n) && hasHeight(n))
        return n.height.raw > 0 ? float(n.width.raw) / float(n.height.raw) : 0;
    return n.ratio > 0 ? n.ratio : 0;
}

// Returns the size of a layer's image in a positioning area of `area` (css-backgrounds-3/#background-size), before
// round repetition adjusts it. An image with no natural dimensions, such as a gradient, takes the area's in each axis
// that its size leaves auto.
PrVec2 imageSize(const LayoutCompounds* compounds, const PrLayoutBackgroundLayer& layer, PrVec2 area) {
    PrLayoutNaturalSize natural = layer.kind == PrLayoutImageKind_Gradient ? prLayoutNaturalSizeNone() : layer.natural;
    float ratio = ratioOf(natural);
    if (layer.size == PrLayoutBackgroundSize_Cover || layer.size == PrLayoutBackgroundSize_Contain) {
        if (ratio <= 0)
            return area;
        bool cover = layer.size == PrLayoutBackgroundSize_Cover;
        bool widthFirst = cover ? area.x / area.y > ratio : area.x / area.y <= ratio;
        return widthFirst ? PrVec2{area.x, area.x / ratio} : PrVec2{area.y * ratio, area.y};
    }
    bool autoX = layoutLengthIsAuto(layer.sizeX);
    bool autoY = layoutLengthIsAuto(layer.sizeY);
    float x = autoX ? 0 : maxF32(resolvePx(compounds, layer.sizeX, area.x), 0);
    float y = autoY ? 0 : maxF32(resolvePx(compounds, layer.sizeY, area.y), 0);
    float naturalX = prLayoutUnitToPx(natural.width);
    float naturalY = prLayoutUnitToPx(natural.height);
    if (autoX && autoY) {
        // The default sizing algorithm (css-images-3/#default-sizing), with the area as the default object size.
        if (hasWidth(natural) && hasHeight(natural))
            return PrVec2{naturalX, naturalY};
        if (hasWidth(natural))
            return PrVec2{naturalX, ratio > 0 ? naturalX / ratio : area.y};
        if (hasHeight(natural))
            return PrVec2{ratio > 0 ? naturalY * ratio : area.x, naturalY};
        if (ratio > 0)
            return area.x / area.y > ratio ? PrVec2{area.y * ratio, area.y} : PrVec2{area.x, area.x / ratio};
        return area;
    }
    if (autoX)
        x = ratio > 0 ? y * ratio : hasWidth(natural) ? naturalX : area.x;
    if (autoY)
        y = ratio > 0 ? x / ratio : hasHeight(natural) ? naturalY : area.y;
    return PrVec2{x, y};
}

// One axis of a layer's tiling: where its first tile starts, the tile's size, the gap between tiles, and whether tiles
// repeat.
struct TileAxis {
    float start;
    float size;
    float spacing;
    bool repeats;
};

// Places one axis of a layer in a positioning area from `low` to `low + extent`, for an image of `size` that its
// repetition has already rounded (css-backgrounds-3/#background-repeat, #background-position).
TileAxis placeAxis(const LayoutCompounds* compounds, PrLayoutBackgroundRepeat repeat, PrLayoutLength position,
        float low, float extent, float size) {
    TileAxis axis{.start = low + resolvePx(compounds, position, extent - size), .size = size};
    switch (repeat) {
        case PrLayoutBackgroundRepeat_Repeat:
        case PrLayoutBackgroundRepeat_Round:
            axis.repeats = true;
            break;
        case PrLayoutBackgroundRepeat_Space: {
            // As many whole copies as fit, the first and last against the area's edges. Fewer than two copies are
            // placed as one, by the position.
            float copies = size > 0 ? floorF32(extent / size) : 0;
            if (copies >= 2) {
                axis.start = low;
                axis.spacing = (extent - copies * size) / (copies - 1);
                axis.repeats = true;
            }
            break;
        }
        default:
            break;
    }
    return axis;
}

// Returns the number of whole copies of `size` that `round` fits in `extent`, at least one.
float roundCopies(float extent, float size) {
    float copies = roundF32(extent / size);
    return copies < 1 ? 1 : copies;
}

// A gradient's stops as draw takes them. Until normalize maps them to the range that the brush spans, each offset is
// the stop's position, as a fraction of the gradient line, ray or turn.
struct ResolvedStops {
    Array<PrDrawColorStop> stops;
    float first;                    // the first color stop's position
    float last;                     // the last color stop's position
};

// Resolves a gradient's stop positions against the length of its line or ray, `length`
// (css-images-4/#color-stop-fixup). A conic gradient's positions are percentages of a turn, with a length of 1.
void resolveStops(const LayoutCompounds* compounds, const PrLayoutGradient& g, float length, ResolvedStops* out) {
    struct Stop {
        float position;
        bool positioned;
        bool hint;
        PrDrawColorStop paint;              // the color and hue
        Array<PrDrawColorStop> followers;   // the colors and hues of the stops at its position that follow it
    };
    auto paintOf = [](const PrLayoutColorStop& s) {
        return PrDrawColorStop{.color = s.color, .hue = s.hue, .hasHue = s.hasHue};
    };
    Array<Stop> stops;
    for (uint32_t i = 0; i < g.stopCount; ++i) {
        const PrLayoutColorStop& s = g.stops[i];
        // A color stop at none follows the color stop before it, and the fixup does not see it.
        if (s.kind == PrLayoutColorStopKind_Color && layoutLengthIsNone(s.position)) {
            if (stops.count != 0 && !stops.last().hint)
                stops.last().followers.push(paintOf(s));
            continue;
        }
        Stop stop{.hint = s.kind == PrLayoutColorStopKind_Hint, .paint = paintOf(s)};
        stop.positioned = !layoutLengthIsAuto(s.position);
        if (stop.positioned && length > 0)
            stop.position = resolvePx(compounds, s.position, length) / length;
        else if (stop.positioned && prLayoutWordKind(s.position.bits) == PrLayoutWordKind_Percent)
            stop.position = prLayoutWordPercentValue(s.position.bits) / 100;
        else if (stop.positioned)
            stop.position = 0;
        // A hint must lie between color stops. One that does not is dropped.
        if (stop.hint && (stops.count == 0 || stops.last().hint || !stop.positioned))
            continue;
        stops.push(stop);
    }
    while (stops.count != 0 && stops.last().hint)
        stops.pop();
    // The first and last color stops default to the ends. A stop before an earlier one moves to it.
    if (stops.count != 0 && !stops.data[0].positioned) {
        stops.data[0].position = 0;
        stops.data[0].positioned = true;
    }
    if (stops.count != 0 && !stops.last().positioned) {
        stops.last().position = 1;
        stops.last().positioned = true;
    }
    float largest = -kInfinityF32;
    for (uint32_t i = 0; i < stops.count; ++i) {
        if (!stops.data[i].positioned)
            continue;
        stops.data[i].position = maxF32(stops.data[i].position, largest);
        largest = stops.data[i].position;
    }
    // A run of color stops with no position is spread evenly between the positioned stops around it.
    for (uint32_t i = 0; i < stops.count;) {
        if (stops.data[i].positioned) {
            ++i;
            continue;
        }
        uint32_t end = i;
        while (!stops.data[end].positioned)
            ++end;
        float from = stops.data[i - 1].position;
        float to = stops.data[end].position;
        for (uint32_t k = i; k < end; ++k)
            stops.data[k].position = from + (to - from) * float(k - i + 1) / float(end - i + 1);
        i = end;
    }
    out->stops.clear();
    out->first = stops.count != 0 ? stops.data[0].position : 0;
    out->last = stops.count != 0 ? stops.last().position : 1;
    for (uint32_t i = 0; i < stops.count; ++i) {
        const Stop& s = stops.data[i];
        if (s.hint) {
            // A hint is a fraction of the gap between the color stops around it. A hint at either stop leaves a hard
            // edge there.
            float previous = stops.data[i - 1].position;
            float gap = stops.data[i + 1].position - previous;
            float fraction = gap > 0 ? (s.position - previous) / gap : 0.5f;
            out->stops.last().hint = clampF32(fraction, 1e-4f, 1 - 1e-4f);
            continue;
        }
        PrDrawColorStop paint = s.paint;
        paint.offset = s.position;
        out->stops.push(paint);
        for (uint32_t f = 0; f < s.followers.count; ++f) {
            paint = s.followers.data[f];
            paint.offset = s.position;
            out->stops.push(paint);
        }
    }
    for (uint32_t i = 0; i < stops.count; ++i)
        stops.data[i].followers.reset();
    stops.reset();
}

// Maps the stops' positions to offsets of the range from `low` to `high`, which the brush spans from 0 to 1.
void normalize(ResolvedStops* resolved, float low, float high) {
    float span = high - low;
    for (uint32_t i = 0; i < resolved->stops.count; ++i) {
        PrDrawColorStop& stop = resolved->stops.data[i];
        stop.offset = (stop.offset - low) / span;
    }
}

// Expands a repeating conic gradient's stops over one whole turn, as offsets of the turn.
bool repeatOverTurn(ResolvedStops* resolved) {
    float period = resolved->last - resolved->first;
    if (!(period > 0))
        return false;
    uint32_t perPeriod = resolved->stops.count;
    float copies = ceilF32(1 / period) + 2;
    if (copies * float(perPeriod) > float(kMaxRepeatedStops))
        return false;
    Array<PrDrawColorStop> turn;
    // The first copy starts at or before the turn's start, and the last ends at or after its end.
    float shift = -period * ceilF32(resolved->first / period) - period;
    for (uint32_t c = 0; float(c) < copies; ++c) {
        for (uint32_t i = 0; i < perPeriod; ++i) {
            PrDrawColorStop stop = resolved->stops.data[i];
            stop.offset += shift + float(c) * period;
            turn.push(stop);
        }
    }
    resolved->stops.reset();
    resolved->stops = turn;
    return true;
}

// The average color of a gradient's stops, by the length of each segment, with straight alpha.
PrColor16 averageColor(const ResolvedStops& resolved) {
    const Array<PrDrawColorStop>& s = resolved.stops;
    float sum[4] = {};
    float total = 0;
    for (uint32_t i = 0; i + 1 < s.count; ++i) {
        float width = s.data[i + 1].offset - s.data[i].offset;
        PrColor a = prColorFromColor16(s.data[i].color);
        PrColor b = prColorFromColor16(s.data[i + 1].color);
        sum[0] += width * (a.r * a.a + b.r * b.a) / 2;
        sum[1] += width * (a.g * a.a + b.g * b.a) / 2;
        sum[2] += width * (a.b * a.a + b.b * b.a) / 2;
        sum[3] += width * (a.a + b.a) / 2;
        total += width;
    }
    if (!(total > 0) && s.count != 0)
        return s.last().color;
    float alpha = sum[3] / total;
    if (!(alpha > 0))
        return PR_COLOR16(0, 0, 0, 0);
    return prColor16FromColor(PrColor{sum[0] / total / alpha, sum[1] / total / alpha, sum[2] / total / alpha, alpha});
}

// The ending shape's radii of a radial gradient centered at `center` in an image of `size`
// (css-images-3/#radial-gradient-syntax).
PrVec2 radialRadii(const LayoutCompounds* compounds, const PrLayoutGradient& g, PrVec2 center, PrVec2 size) {
    bool circle = g.shape == PrLayoutRadialShape_Circle;
    if (g.extent == PrLayoutRadialExtent_Radii) {
        float rx = maxF32(resolvePx(compounds, g.radiusX, size.x), 0);
        float ry = circle ? rx : maxF32(resolvePx(compounds, g.radiusY, size.y), 0);
        return PrVec2{rx, ry};
    }
    float left = absF32(center.x);
    float right = absF32(size.x - center.x);
    float top = absF32(center.y);
    float bottom = absF32(size.y - center.y);
    bool closest = g.extent == PrLayoutRadialExtent_ClosestSide || g.extent == PrLayoutRadialExtent_ClosestCorner;
    float sideX = closest ? minF32(left, right) : maxF32(left, right);
    float sideY = closest ? minF32(top, bottom) : maxF32(top, bottom);
    bool side = g.extent == PrLayoutRadialExtent_ClosestSide || g.extent == PrLayoutRadialExtent_FarthestSide;
    if (side) {
        if (circle) {
            float r = closest ? minF32(sideX, sideY) : maxF32(sideX, sideY);
            return PrVec2{r, r};
        }
        return PrVec2{sideX, sideY};
    }
    // A corner: the circle through it, or the ellipse through it with the aspect ratio of the same side ellipse.
    if (circle) {
        float r = sqrtF32(sideX * sideX + sideY * sideY);
        return PrVec2{r, r};
    }
    if (sideX <= 0 || sideY <= 0)
        return PrVec2{sideX, sideY};
    float ratio = sideX / sideY;
    float ry = sqrtF32(sideX * sideX / (ratio * ratio) + sideY * sideY);
    return PrVec2{ry * ratio, ry};
}

// Returns the brush of a gradient drawn over `tile`. The brush's stops are in `resolved`, which must outlive it.
// Returns false if the gradient paints one color, written to `solid`.
//
// A brush pads past its range by clamping its parameter to 0 and 1, so its range covers every stop: a linear gradient
// spans its line and its stops beyond the line, a radial gradient spans its ray from the center, and a conic gradient
// spans its turn. A repeating gradient spans one period instead.
bool gradientBrush(const LayoutCompounds* compounds, const PrLayoutGradient& g, PrRect tile, ResolvedStops* resolved,
        PrDrawBrush* out, PrColor16* solid) {
    PrVec2 size{tile.right - tile.left, tile.bottom - tile.top};
    PrVec2 origin{tile.left, tile.top};
    float length = 1;
    PrVec2 direction{};
    PrVec2 center{};
    PrVec2 radii{};
    float degrees = g.angle;
    switch (g.kind) {
        case PrLayoutGradientKind_Linear: {
            if (g.corner != PrLayoutGradientCorner_None) {
                float toward = atan2F32(size.y, size.x) * 180 / kPiF32;
                switch (g.corner) {
                    case PrLayoutGradientCorner_TopRight: degrees = toward; break;
                    case PrLayoutGradientCorner_BottomRight: degrees = 180 - toward; break;
                    case PrLayoutGradientCorner_BottomLeft: degrees = 180 + toward; break;
                    default: degrees = 360 - toward; break;
                }
            }
            float radians = degrees * kPiF32 / 180;
            direction = PrVec2{sinF32(radians), -cosF32(radians)};
            length = absF32(size.x * direction.x) + absF32(size.y * direction.y);
            break;
        }
        case PrLayoutGradientKind_Radial: {
            center = PrVec2{resolvePx(compounds, g.centerX, size.x), resolvePx(compounds, g.centerY, size.y)};
            radii = radialRadii(compounds, g, center, size);
            // A degenerate ellipse is drawn as a very thin one (css-images-3/#degenerate-radials).
            constexpr float kThin = 1.0f / 1024;
            radii = PrVec2{maxF32(radii.x, kThin), maxF32(radii.y, kThin)};
            length = radii.x;
            break;
        }
        default:
            center = PrVec2{resolvePx(compounds, g.centerX, size.x), resolvePx(compounds, g.centerY, size.y)};
            break;
    }
    resolveStops(compounds, g, length, resolved);
    if (resolved->stops.count == 0) {
        *solid = PR_COLOR16(0, 0, 0, 0);
        return false;
    }
    float period = resolved->last - resolved->first;
    if (g.repeating && !(period > 0)) {
        *solid = averageColor(*resolved);
        return false;
    }
    PrDrawBrush brush{};
    switch (g.kind) {
        case PrLayoutGradientKind_Linear: {
            float low = g.repeating ? resolved->first : minF32(resolved->first, 0);
            float high = g.repeating ? resolved->last : maxF32(resolved->last, 1);
            normalize(resolved, low, high);
            PrVec2 middle = origin + size * 0.5f;
            PrVec2 start = middle + direction * (length * (low - 0.5f));
            PrVec2 end = middle + direction * (length * (high - 0.5f));
            brush = prDrawBrushLinear(start, end, resolved->stops.data, resolved->stops.count);
            break;
        }
        case PrLayoutGradientKind_Radial: {
            // A ray has no negative radius, so a repeating gradient moves its period out by whole periods.
            float low = 0;
            float high = maxF32(resolved->last, 1);
            if (g.repeating) {
                float shift = resolved->first < 0 ? period * ceilF32(-resolved->first / period) : 0;
                low = resolved->first + shift;
                high = resolved->last + shift;
                for (uint32_t i = 0; i < resolved->stops.count; ++i)
                    resolved->stops.data[i].offset += shift;
            }
            normalize(resolved, low, high);
            PrVec2 at = origin + center;
            brush = prDrawBrushRadial(at, radii.x * low, at, radii.x * high, resolved->stops.data,
                resolved->stops.count);
            // An ellipse is the circle of its horizontal radius, scaled vertically about its center.
            if (radii.y != radii.x)
                brush.transform = prAffine2Concat(prAffine2Concat(prAffine2Translate(PrVec2{-at.x, -at.y}),
                    prAffine2Scale(PrVec2{1, radii.y / radii.x})), prAffine2Translate(at));
            break;
        }
        default:
            if (g.repeating && !repeatOverTurn(resolved)) {
                *solid = averageColor(*resolved);
                return false;
            }
            // The turn starts up, at the gradient's angle.
            brush = prDrawBrushConic(origin + center, (degrees - 90) * kPiF32 / 180, resolved->stops.data,
                resolved->stops.count);
            break;
    }
    brush.extend = g.repeating && g.kind != PrLayoutGradientKind_Conic ? PrDrawExtend_Repeat : PrDrawExtend_Pad;
    brush.interpolationSpace = g.space;
    brush.hueMethod = g.hueMethod;
    *out = brush;
    return true;
}

// Records one tile of a gradient into `list` over `rect`, drawn as it is over `tile`, with `rect`'s edges as `edges`
// says, compositing by `blendMode` and `composite`.
void drawGradient(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutGradient& g, PrRect tile, PrRect rect,
        PrDrawEdges edges, PrDrawList* list, PrDrawBlendMode blendMode = PrDrawBlendMode_Normal,
        PrDrawComposite composite = PrDrawComposite_SourceOver) {
    ResolvedStops resolved{};
    PrDrawBrush brush{};
    PrColor16 solid{};
    if (gradientBrush(compounds, g, tile, &resolved, &brush, &solid))
        prDrawListRect(list, rect, {.brush = &brush, .composite = composite, .blendMode = blendMode, .edges = edges});
    else if (prFloat16ToFloat(solid.a) > 0)
        prDrawListRect(list, rect, {.color = solid, .composite = composite, .blendMode = blendMode, .edges = edges});
    resolved.stops.reset();
}

// Returns how a layer's image is sampled where it is drawn at another size.
PrDrawSampling samplingOf(const PrLayoutBackgroundLayer& layer) {
    if (layer.rendering == PrLayoutImageRendering_Auto || layer.kind == PrLayoutImageKind_Gradient)
        return PrDrawSampling_Linear;
    return PrDrawSampling_Nearest;
}

// Returns the source of a layer's image drawn at `size`, before its filter list. A list that the source reads may be
// written to `drawn`, which the caller releases.
PrDrawSource unfilteredSource(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutBackgroundLayer& layer,
        PrVec2 size, PrDrawList* PR_NULLABLE* PR_NONNULL drawn) {
    PrRect own{0, 0, size.x, size.y};
    switch (layer.kind) {
        case PrLayoutImageKind_Image: {
            PrUVec2 texels = prDrawImageExtent(layer.image);
            return prDrawSourceImage(layer.image, PrRect{0, 0, float(texels.x), float(texels.y)});
        }
        case PrLayoutImageKind_List: {
            float width = hasWidth(layer.natural) ? prLayoutUnitToPx(layer.natural.width) : size.x;
            float height = hasHeight(layer.natural) ? prLayoutUnitToPx(layer.natural.height) : size.y;
            return prDrawSourceList(layer.list, PrRect{0, 0, width, height});
        }
        case PrLayoutImageKind_Painter:
            // A painter draws at the size that it is shown at, rather than being scaled to it.
            *drawn = prDrawListCreate();
            layer.painter.paint(layer.painter.context, size, *drawn);
            return prDrawSourceList(*drawn, own);
        default:
            *drawn = prDrawListCreate();
            drawGradient(compounds, layer.gradient, own, own, PrDrawEdges_Antialiased, *drawn);
            return prDrawSourceList(*drawn, own);
    }
}

// Returns the filter of a layer's filter list for its image drawn over `own`, or null if a source of the list draws
// nothing. The filter region is `own`, within the region of a source that ends the list
// (filter-effects-1/#FilterCSSImageValue).
const PrDrawFilter* PR_NULLABLE imageFilterOf(const PrLayoutBackgroundLayer& layer, PrRect own) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterInput at = PrDrawFilterInput_Source;
    PrRect region = own;
    bool hidden = false;
    for (uint32_t i = 0; i < layer.filterCount && !hidden; ++i) {
        const PrLayoutFilter& f = layer.filters[i];
        region = own;
        if (f.kind != PrLayoutFilterKind_Source) {
            at = layoutFilterAppend(f, at, PrImageEdgeMode_Duplicate, builder);
            continue;
        }
        const PrLayoutSourceFunctions* functions = f.source.functions;
        if (functions == nullptr || functions->filter == nullptr)
            continue;
        PrLayoutSourceFilter result{.result = at};
        hidden = !functions->filter(f.source.context, own, own, at, builder, &result);
        at = result.result;
        if (result.region.right > result.region.left && result.region.bottom > result.region.top)
            region = prRectIntersect(own, result.region);
    }
    // A list whose items added no node filters as the identity.
    if (at == PrDrawFilterInput_Source) {
        PrDrawFilterNode node{};
        node.kind = PrDrawFilterKind_Offset;
        node.space = PrColorSpace_Srgb;
        node.inputs[0] = at;
        prDrawFilterBuilderAdd(builder, &node);
    }
    prDrawFilterBuilderSetRegion(builder, region);
    const PrDrawFilter* filter = hidden ? nullptr : prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

bool isEmpty(PrRect r) {
    return !(r.right > r.left && r.bottom > r.top);
}

// Records one layer. Returns true if it drew.
bool paintLayer(const LayoutCompounds* compounds, const PrLayoutBackgroundLayer& layer,
        const LayoutBackgroundBoxes& boxes, PrDrawList* list) {
    if (layer.kind == PrLayoutImageKind_None)
        return false;
    PrRect area = boxes.boxes[layer.origin];
    PrRect painted = isEmpty(boxes.area) ? boxes.boxes[layer.clip] : boxes.area;
    PrVec2 extent{area.right - area.left, area.bottom - area.top};
    if (isEmpty(painted) || !(extent.x > 0) || !(extent.y > 0))
        return false;
    PrVec2 size = imageSize(compounds, layer, extent);
    // Round scales the image so that whole copies fill the area. If only one axis rounds and the other is auto, the
    // other keeps the image's ratio.
    bool roundX = layer.repeatX == PrLayoutBackgroundRepeat_Round && size.x > 0;
    bool roundY = layer.repeatY == PrLayoutBackgroundRepeat_Round && size.y > 0;
    bool lengths = layer.size == PrLayoutBackgroundSize_Lengths;
    if (roundX) {
        float rounded = extent.x / roundCopies(extent.x, size.x);
        if (!roundY && lengths && layoutLengthIsAuto(layer.sizeY))
            size.y *= rounded / size.x;
        size.x = rounded;
    }
    if (roundY) {
        float rounded = extent.y / roundCopies(extent.y, size.y);
        if (!roundX && lengths && layoutLengthIsAuto(layer.sizeX))
            size.x *= rounded / size.y;
        size.y = rounded;
    }
    if (!(size.x > 0) || !(size.y > 0))
        return false;
    TileAxis x = placeAxis(compounds, layer.repeatX, layer.positionX, area.left, extent.x, size.x);
    TileAxis y = placeAxis(compounds, layer.repeatY, layer.positionY, area.top, extent.y, size.y);
    PrRect tile{x.start, y.start, x.start + size.x, y.start + size.y};
    PrRect rect = painted;
    if (!x.repeats) {
        rect.left = maxF32(rect.left, tile.left);
        rect.right = minF32(rect.right, tile.right);
    }
    if (!y.repeats) {
        rect.top = maxF32(rect.top, tile.top);
        rect.bottom = minF32(rect.bottom, tile.bottom);
    }
    if (isEmpty(rect))
        return false;
    // A tile that covers what paints draws once, without tiling.
    bool once =
        tile.left <= rect.left && tile.right >= rect.right && tile.top <= rect.top && tile.bottom >= rect.bottom;
    if (layer.kind == PrLayoutImageKind_Gradient && layer.filterCount == 0 && once) {
        drawGradient(compounds, layer.gradient, tile, rect, PrDrawEdges_Snapped, list, layer.blendMode,
            layer.composite);
        return true;
    }
    PrDrawList* drawn = nullptr;
    PrDrawSource source = layoutImageSource(compounds, layer, size, &drawn);
    PrDrawSampling sampling = samplingOf(layer);
    PrDrawBrush brush = prDrawBrushImage(source, tile, sampling);
    brush.image.tiles.spacing = PrVec2{x.spacing, y.spacing};
    // An axis that does not repeat pads its edge texels, so that a pixel that the tile's edge covers in part samples
    // the tile. The rect stops at the tile in that axis. Each copy of a repeated image is filtered on its own, because
    // each tile of a background is a copy of the image (css-backgrounds-3/#background-repeat).
    brush.image.tiles.x = x.repeats ? PrDrawExtend_Repeat : PrDrawExtend_Pad;
    brush.image.tiles.y = y.repeats ? PrDrawExtend_Repeat : PrDrawExtend_Pad;
    brush.image.tiles.flags = PrDrawTileFlag_Clamped;
    if (layer.kind == PrLayoutImageKind_Image && layer.filterCount == 0)
        brush.image.limit = layer.limit;
    prDrawListRect(list, rect,
        {.brush = &brush, .composite = layer.composite, .blendMode = layer.blendMode, .edges = PrDrawEdges_Snapped});
    prDrawListRelease(drawn);
    return true;
}

} // namespace

void layoutBackgroundsInit(LayoutBackgrounds* backgrounds) {
    backgrounds->entries.push(LayoutBackground{});
}

PrLayoutBackgroundIndex layoutBackgroundsIntern(LayoutBackgrounds* backgrounds, LayoutCompounds* compounds,
        const PrLayoutBackgroundLayer* layers, uint32_t count) {
    if (count == 0)
        return kPrLayoutBackgroundNone;
    Array<uint8_t> key;
    backgroundKey(key, layers, count);
    uint32_t hash = uint32_t(hashBytes(key.data, key.count, kBackgroundSeed));
    uint32_t found = backgrounds->index.find(hash, [&](uint32_t i) {
        const LayoutBackground& b = backgrounds->entries.data[i];
        return b.key.count == key.count && memEquals(b.key.data, key.data, key.count);
    });
    if (found != kHashMissing) {
        key.reset();
        return found;
    }
    uint32_t at = 0;
    if (backgrounds->free.count != 0) {
        at = backgrounds->free.last();
        backgrounds->free.pop();
    } else {
        at = backgrounds->entries.count;
        backgrounds->entries.push(LayoutBackground{});
    }
    uint32_t stopCount = 0;
    uint32_t filterCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (layers[i].kind == PrLayoutImageKind_Gradient)
            stopCount += layers[i].gradient.stopCount;
        filterCount += layers[i].filterCount;
    }
    LayoutBackground& entry = backgrounds->entries.data[at];
    entry.layers = static_cast<PrLayoutBackgroundLayer*>(prMemoryAlloc(sizeof(PrLayoutBackgroundLayer) * count));
    memCopy(entry.layers, layers, sizeof(PrLayoutBackgroundLayer) * count);
    entry.count = count;
    entry.stops =
        stopCount != 0 ? static_cast<PrLayoutColorStop*>(prMemoryAlloc(sizeof(PrLayoutColorStop) * stopCount)) : nullptr;
    entry.filters =
        filterCount != 0 ? static_cast<PrLayoutFilter*>(prMemoryAlloc(sizeof(PrLayoutFilter) * filterCount)) : nullptr;
    uint32_t nextStop = 0;
    uint32_t nextFilter = 0;
    for (uint32_t i = 0; i < count; ++i) {
        PrLayoutBackgroundLayer& l = entry.layers[i];
        if (l.kind == PrLayoutImageKind_List)
            prDrawListFreeze(l.list);
        layoutImageTakeFilters(l, entry.filters, &nextFilter);
        if (l.kind != PrLayoutImageKind_Gradient)
            continue;
        memCopy(entry.stops + nextStop, l.gradient.stops, sizeof(PrLayoutColorStop) * l.gradient.stopCount);
        l.gradient.stops = entry.stops + nextStop;
        nextStop += l.gradient.stopCount;
    }
    entry.blends = layoutBackgroundBlends(entry.layers, count);
    entry.key = key;
    entry.hash = hash;
    entry.references = 0;
    countLayers(compounds, entry.layers, count, 1);
    backgrounds->index.insert(hash, at);
    backgrounds->candidates.push(at);
    return at;
}

void layoutBackgroundsCount(LayoutBackgrounds* backgrounds, PrLayoutBackgroundIndex index, uint32_t delta) {
    if (index == kPrLayoutBackgroundNone)
        return;
    LayoutBackground& b = backgrounds->entries.data[index];
    b.references += delta;
    if (b.references == 0)
        backgrounds->candidates.push(index);
}

void layoutBackgroundsCollect(LayoutBackgrounds* backgrounds, LayoutCompounds* compounds) {
    for (uint32_t k = 0; k < backgrounds->candidates.count; ++k) {
        uint32_t i = backgrounds->candidates.data[k];
        LayoutBackground& b = backgrounds->entries.data[i];
        if (b.layers == nullptr || b.references != 0)
            continue;
        backgrounds->index.erase(b.hash, i);
        countLayers(compounds, b.layers, b.count, 0xffffffffu);
        freeEntry(b);
        backgrounds->free.push(i);
    }
    backgrounds->candidates.clear();
}

bool layoutBackgroundsVerify(const LayoutBackgrounds* backgrounds) {
    return layoutCandidatesCover(backgrounds->candidates, backgrounds->entries.count, [&](uint32_t i) {
        const LayoutBackground& b = backgrounds->entries.data[i];
        return b.layers != nullptr && b.references == 0;
    });
}

void layoutBackgroundsReset(LayoutBackgrounds* backgrounds) {
    for (uint32_t i = 0; i < backgrounds->entries.count; ++i) {
        LayoutBackground& b = backgrounds->entries.data[i];
        if (b.layers == nullptr)
            continue;
        for (uint32_t l = 0; l < b.count; ++l) {
            if (b.layers[l].kind == PrLayoutImageKind_Image)
                prDrawImageRelease(b.layers[l].image);
            else if (b.layers[l].kind == PrLayoutImageKind_List)
                prDrawListRelease(b.layers[l].list);
            else if (b.layers[l].kind == PrLayoutImageKind_Painter)
                prLayoutPainterRelease(b.layers[l].painter);
            for (uint32_t f = 0; f < b.layers[l].filterCount; ++f)
                layoutSourceCount(b.layers[l].filters[f].source, 0xffffffffu);
        }
        freeEntry(b);
    }
    backgrounds->entries.reset();
    backgrounds->free.reset();
    backgrounds->candidates.reset();
    backgrounds->index.reset();
}

void layoutBackgroundKey(Array<uint8_t>& key, const PrLayoutBackgroundLayer* layers, uint32_t count) {
    backgroundKey(key, layers, count);
}

void layoutBackgroundCountLayers(LayoutCompounds* compounds, const PrLayoutBackgroundLayer* layers, uint32_t count,
        uint32_t delta) {
    for (uint32_t i = 0; delta == 1 && i < count; ++i) {
        if (layers[i].kind == PrLayoutImageKind_List)
            prDrawListFreeze(layers[i].list);
    }
    countLayers(compounds, layers, count, delta);
}

void layoutBackgroundDrawGradient(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutGradient& g, PrRect box,
        PrRect rect, PrDrawEdges edges, PrDrawList* list) {
    drawGradient(compounds, g, box, rect, edges, list);
}

void layoutBackgroundRound(LayoutBackgroundBoxes& boxes, const PrCornerRect& border, uint32_t used) {
    boxes.rounded = true;
    const PrRect& outer = border.rect;
    for (uint32_t b = 0; b < 3; ++b) {
        if ((used & (1u << b)) == 0)
            continue;
        const PrRect& inner = boxes.boxes[b];
        float insets[PrRectSide_Count] = {
            inner.top - outer.top, outer.right - inner.right,
            outer.bottom - inner.bottom, inner.left - outer.left,
        };
        bool same = insets[0] == 0 && insets[1] == 0 && insets[2] == 0 && insets[3] == 0;
        boxes.shapes[b] = same ? border : prCornerRectInset(border, insets);
    }
}

bool layoutBackgroundPaintLayer(const LayoutCompounds* compounds, const PrLayoutBackgroundLayer& layer,
        const LayoutBackgroundBoxes& boxes, PrDrawList* list) {
    if (!boxes.rounded || !isEmpty(boxes.area))
        return paintLayer(compounds, layer, boxes, list);
    // The layer is clipped to the curve of its box as well as to the box. The clip is recorded whether or not the layer
    // paints, so the layer counts as recorded.
    prDrawListPushClipCornerRect(list, boxes.shapes[layer.clip], PrDrawEdges_Snapped);
    paintLayer(compounds, layer, boxes, list);
    prDrawListPopClip(list);
    return true;
}

void layoutImageTakeFilters(PrLayoutBackgroundLayer& image, PrLayoutFilter* PR_NULLABLE filters, uint32_t* next) {
    if (image.filterCount == 0) {
        image.filters = nullptr;
        return;
    }
    memCopy(filters + *next, image.filters, sizeof(PrLayoutFilter) * image.filterCount);
    image.filters = filters + *next;
    *next += image.filterCount;
}

PrDrawSource layoutImageSource(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutBackgroundLayer& layer,
        PrVec2 size, PrDrawList* PR_NULLABLE* PR_NONNULL drawn) {
    *drawn = nullptr;
    PrDrawSource source = unfilteredSource(compounds, layer, size, drawn);
    if (layer.filterCount == 0)
        return source;
    PrRect own{0, 0, size.x, size.y};
    PrDrawList* filtered = prDrawListCreate();
    const PrDrawFilter* filter = imageFilterOf(layer, own);
    if (filter != nullptr) {
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = own;
        group.filter = filter;
        prDrawListPushGroup(filtered, &group);
        PrDrawBrush brush = prDrawBrushImage(source, own, samplingOf(layer));
        brush.image.tiles.x = PrDrawExtend_Pad;
        brush.image.tiles.y = PrDrawExtend_Pad;
        brush.image.tiles.flags = PrDrawTileFlag_Clamped;
        if (layer.kind == PrLayoutImageKind_Image)
            brush.image.limit = layer.limit;
        prDrawListRect(filtered, own, {.brush = &brush});
        prDrawListPopGroup(filtered);
        prDrawFilterRelease(filter);
    }
    prDrawListRelease(*drawn);
    *drawn = filtered;
    return prDrawSourceList(filtered, own);
}

bool layoutBackgroundBlends(const PrLayoutBackgroundLayer* layers, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (layers[i].blendMode != PrDrawBlendMode_Normal || layers[i].composite != PrDrawComposite_SourceOver)
            return true;
    }
    return false;
}

void layoutSourceKey(Array<uint8_t>& key, const PrLayoutSource& source) {
    appendKey(key, source.functions);
    appendKey(key, source.context);
    appendKey(key, source.version);
}

void layoutSourceCount(const PrLayoutSource& source, uint32_t delta) {
    if (source.functions == nullptr)
        return;
    if (delta == 1 && source.functions->retain != nullptr)
        source.functions->retain(source.context);
    else if (delta != 1 && source.functions->release != nullptr)
        source.functions->release(source.context);
}

void layoutFilterKey(Array<uint8_t>& key, const PrLayoutFilter* PR_NULLABLE filters, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrLayoutFilter& f = filters[i];
        appendKey(key, f.kind);
        appendKey(key, f.amount);
        appendKey(key, f.offset);
        appendKey(key, f.sigma);
        appendKey(key, f.color);
        layoutSourceKey(key, f.source);
    }
}

} // namespace pr_impl
