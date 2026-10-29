// Color glyphs.

#include "font/src/colr.h"

#include "pr/float.h"
#include "pr/search.h"

namespace pr_impl {

namespace {

constexpr uint32_t kColr = 0x434f4c52;           // 'COLR'
constexpr uint32_t kCpal = 0x4350414c;           // 'CPAL'

// The maximum nesting depth of the paints of a color glyph. It bounds the walk for a font whose graph has a cycle.
constexpr uint32_t kPaintMaxDepth = 64;

// The maximum number of paints that the walk of a color glyph visits. A graph that shares paints can reach
// exponentially many within the depth limit.
constexpr uint32_t kPaintMaxVisits = 65536;

// The palette index with which COLR asks for the text color.
constexpr uint16_t kForegroundIndex = 0xffff;

// The variation index base that varies nothing.
constexpr uint32_t kNoVariation = 0xffffffffu;

// The most deltas that one paint reads.
constexpr uint32_t kMaxDeltas = 6;

// The formats of the paints of COLR version 1. A variable format is the format after its static one.
enum {
    PaintFormat_ColrLayers = 1,
    PaintFormat_Solid = 2,
    PaintFormat_LinearGradient = 4,
    PaintFormat_RadialGradient = 6,
    PaintFormat_SweepGradient = 8,
    PaintFormat_Glyph = 10,
    PaintFormat_ColrGlyph = 11,
    PaintFormat_Transform = 12,
    PaintFormat_Translate = 14,
    PaintFormat_Scale = 16,
    PaintFormat_ScaleAroundCenter = 18,
    PaintFormat_ScaleUniform = 20,
    PaintFormat_ScaleUniformAroundCenter = 22,
    PaintFormat_Rotate = 24,
    PaintFormat_RotateAroundCenter = 26,
    PaintFormat_Skew = 28,
    PaintFormat_SkewAroundCenter = 30,
    PaintFormat_Composite = 32,
};
typedef uint8_t PaintFormat;

// The state of one walk.
struct Walker {
    const ColrFace& face;
    SfntCoords coords;
    ColrWalk& walk;
    uint32_t visits;                // the number of paints left to visit, out of kPaintMaxVisits
};

// Returns the 24-bit offset at `at`.
uint32_t offset24(SfntTable t, uint32_t at) {
    return uint32_t(t.uint8At(at)) << 16 | uint32_t(t.uint16At(at + 1));
}

// Gets the deltas of `count` successive variation indices from `base`, each rounded to a whole number of its field's
// units. A base of kNoVariation, or the default instance, gives zeros.
void deltasOf(const Walker& w, uint32_t base, uint32_t count, float* out) {
    for (uint32_t i = 0; i < count; ++i)
        out[i] = 0;
    if (base == kNoVariation || w.coords.count == 0 || w.face.variations.size == 0)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = base + i;
        uint16_t outer = uint16_t(index >> 16);
        uint16_t inner = uint16_t(index & 0xffff);
        if (w.face.variationMap.size != 0)
            sfntDeltaSetIndex(w.face.variationMap, index, &outer, &inner);
        float delta = sfntVariationDelta(w.face.variations, outer, inner, w.coords);
        out[i] = roundF32(delta);
    }
}

PrColor colorOf(const Walker& w, uint16_t index, float alpha) {
    PrColor color{0, 0, 0, 0};
    for (uint32_t i = w.walk.overrideCount; i > 0; --i) {
        if (index != kForegroundIndex && w.walk.overrides[i - 1].index == index) {
            color = w.walk.overrides[i - 1].color;
            color.a *= alpha;
            return color;
        }
    }
    if (index == kForegroundIndex)
        color = w.walk.foreground;
    else
        color = colrPaletteEntry(w.face, w.walk.palette, index);
    color.a *= alpha;
    return color;
}

// Converts a point in font units with y up to ems with y down.
PrVec2 pointFrom(const Walker& w, float x, float y) {
    return PrVec2{x / w.walk.unitsPerEm, -y / w.walk.unitsPerEm};
}

// Converts a transform of font units with y up to the same transform of ems with y down.
PrAffine2 transformFrom(const Walker& w, float xx, float yx, float xy, float yy, float dx, float dy) {
    return PrAffine2{xx, -yx, -xy, yy, dx / w.walk.unitsPerEm, -dy / w.walk.unitsPerEm};
}

// Returns `inner` about the point (cx, cy) in font units.
PrAffine2 aboutCenter(const Walker& w, PrAffine2 inner, float cx, float cy) {
    PrVec2 c = pointFrom(w, cx, cy);
    return prAffine2Concat(prAffine2Concat(prAffine2Translate(PrVec2{-c.x, -c.y}), inner), prAffine2Translate(c));
}

void addStep(Walker& w, ColrOp op) {
    ColrStep step{};
    step.op = op;
    w.walk.steps.push(step);
}

void walkPaint(Walker& w, SfntTable paint, uint32_t depth);

// Returns the paint at a 24-bit offset from `paint`, or an empty table for offset 0.
SfntTable child(SfntTable paint, uint32_t at) {
    uint32_t to = offset24(paint, at);
    return to != 0 ? paint.from(to) : SfntTable{nullptr, 0};
}

void pushTransform(Walker& w, PrAffine2 transform, SfntTable next, uint32_t depth) {
    ColrStep step{};
    step.op = ColrOp_PushTransform;
    step.transform = transform;
    w.walk.steps.push(step);
    walkPaint(w, next, depth + 1);
    addStep(w, ColrOp_PopTransform);
}

// Appends the stops of the color line at `line`, sorted by offset, to the stops of the walk.
void addStops(Walker& w, SfntTable line, bool variable, ColrStep& step) {
    step.stopStart = w.walk.stops.count;
    uint8_t extend = line.uint8At(0);
    uint32_t count = line.uint16At(1);
    uint32_t size = variable ? 10 : 6;
    for (uint32_t i = 0; i < count && 3 + (i + 1) * size <= line.size; ++i) {
        uint32_t at = 3 + i * size;
        float offset = line.f2dot14(at);
        float alpha = line.f2dot14(at + 4);
        if (variable) {
            float d[2];
            deltasOf(w, line.uint32At(at + 6), 2, d);
            offset += d[0] / 16384;
            alpha += d[1] / 16384;
        }
        PrFontColorStop stop{offset, colorOf(w, line.uint16At(at + 2), alpha)};
        uint32_t position = w.walk.stops.count;
        while (position > step.stopStart && w.walk.stops.data[position - 1].offset > stop.offset)
            --position;
        w.walk.stops.insert(position, stop);
    }
    step.paint.stopCount = w.walk.stops.count - step.stopStart;
    step.paint.extend = extend == 1 ? PrFontExtend_Repeat : extend == 2 ? PrFontExtend_Reflect : PrFontExtend_Pad;
}

// Returns the paint of the glyph from version 1's BaseGlyphList, or an empty table if it has none.
SfntTable basePaint(const ColrFace& face, uint32_t glyph) {
    const SfntTable& list = face.baseGlyphs;
    uint32_t count = list.uint32At(0);
    uint32_t i = partitionPoint(0, count, [&](uint32_t k) { return list.uint16At(4 + k * 6) < glyph; });
    if (i == count || list.uint16At(4 + i * 6) != glyph)
        return SfntTable{nullptr, 0};
    return list.from(list.uint32At(4 + i * 6 + 2));
}

void walkPaint(Walker& w, SfntTable paint, uint32_t depth) {
    if (paint.size == 0 || depth > kPaintMaxDepth || w.visits == 0)
        return;
    --w.visits;
    PaintFormat format = paint.uint8At(0);
    bool variable = format >= 3 && format <= 31 && (format % 2) == 1 && format != PaintFormat_ColrGlyph;
    PaintFormat base = variable ? PaintFormat(format - 1) : format;
    float d[kMaxDeltas] = {};
    switch (base) {
        case PaintFormat_ColrLayers: {
            uint32_t count = paint.uint8At(1);
            uint32_t first = paint.uint32At(2);
            const SfntTable& layers = w.face.layers;
            for (uint32_t i = 0; i < count && first + i < layers.uint32At(0); ++i)
                walkPaint(w, layers.from(layers.uint32At(4 + (first + i) * 4)), depth + 1);
            break;
        }
        case PaintFormat_Solid: {
            float alpha = paint.f2dot14(3);
            if (variable) {
                deltasOf(w, paint.uint32At(5), 1, d);
                alpha += d[0] / 16384;
            }
            ColrStep step{};
            step.op = ColrOp_Fill;
            step.paint.kind = PrFontPaintKind_Solid;
            step.paint.color = colorOf(w, paint.uint16At(1), alpha);
            w.walk.steps.push(step);
            break;
        }
        case PaintFormat_LinearGradient: {
            float p[6];
            for (uint32_t i = 0; i < 6; ++i)
                p[i] = float(paint.int16At(4 + i * 2));
            if (variable) {
                deltasOf(w, paint.uint32At(16), 6, d);
                for (uint32_t i = 0; i < 6; ++i)
                    p[i] += d[i];
            }
            ColrStep step{};
            step.op = ColrOp_Fill;
            step.paint.kind = PrFontPaintKind_Linear;
            addStops(w, paint.from(offset24(paint, 1)), variable, step);
            step.paint.linear.start = pointFrom(w, p[0], p[1]);
            step.paint.linear.end = pointFrom(w, p[2], p[3]);
            step.paint.linear.rotationPoint = pointFrom(w, p[4], p[5]);
            w.walk.steps.push(step);
            break;
        }
        case PaintFormat_RadialGradient: {
            float p[6] = {float(paint.int16At(4)), float(paint.int16At(6)), float(paint.uint16At(8)),
                          float(paint.int16At(10)), float(paint.int16At(12)), float(paint.uint16At(14))};
            if (variable) {
                deltasOf(w, paint.uint32At(16), 6, d);
                for (uint32_t i = 0; i < 6; ++i)
                    p[i] += d[i];
            }
            ColrStep step{};
            step.op = ColrOp_Fill;
            step.paint.kind = PrFontPaintKind_Radial;
            addStops(w, paint.from(offset24(paint, 1)), variable, step);
            step.paint.radial.start = pointFrom(w, p[0], p[1]);
            step.paint.radial.startRadius = p[2] / w.walk.unitsPerEm;
            step.paint.radial.end = pointFrom(w, p[3], p[4]);
            step.paint.radial.endRadius = p[5] / w.walk.unitsPerEm;
            w.walk.steps.push(step);
            break;
        }
        case PaintFormat_SweepGradient: {
            float cx = float(paint.int16At(4));
            float cy = float(paint.int16At(6));
            float start = paint.f2dot14(8);
            float end = paint.f2dot14(10);
            if (variable) {
                deltasOf(w, paint.uint32At(12), 4, d);
                cx += d[0];
                cy += d[1];
                start += d[2] / 16384;
                end += d[3] / 16384;
            }
            ColrStep step{};
            step.op = ColrOp_Fill;
            step.paint.kind = PrFontPaintKind_Conic;
            addStops(w, paint.from(offset24(paint, 1)), variable, step);
            // COLR stores the angles of a sweep in half turns minus one, counterclockwise with y up. That is the
            // negative of clockwise with y down.
            step.paint.conic.center = pointFrom(w, cx, cy);
            step.paint.conic.startAngle = -(start + 1) * kPiF32;
            step.paint.conic.endAngle = -(end + 1) * kPiF32;
            w.walk.steps.push(step);
            break;
        }
        case PaintFormat_Glyph: {
            ColrStep step{};
            step.op = ColrOp_PushClipGlyph;
            step.glyph = paint.uint16At(4);
            w.walk.steps.push(step);
            walkPaint(w, child(paint, 1), depth + 1);
            addStep(w, ColrOp_PopClip);
            break;
        }
        case PaintFormat_ColrGlyph:
            walkPaint(w, basePaint(w.face, paint.uint16At(1)), depth + 1);
            break;
        case PaintFormat_Transform: {
            SfntTable affine = paint.from(offset24(paint, 4));
            float m[6];
            for (uint32_t i = 0; i < 6; ++i)
                m[i] = affine.fixed(i * 4);
            if (variable) {
                deltasOf(w, affine.uint32At(24), 6, d);
                for (uint32_t i = 0; i < 6; ++i)
                    m[i] += d[i] / 65536;
            }
            pushTransform(w, transformFrom(w, m[0], m[1], m[2], m[3], m[4], m[5]), child(paint, 1), depth);
            break;
        }
        case PaintFormat_Translate: {
            float dx = float(paint.int16At(4));
            float dy = float(paint.int16At(6));
            if (variable) {
                deltasOf(w, paint.uint32At(8), 2, d);
                dx += d[0];
                dy += d[1];
            }
            pushTransform(w, transformFrom(w, 1, 0, 0, 1, dx, dy), child(paint, 1), depth);
            break;
        }
        case PaintFormat_Scale:
        case PaintFormat_ScaleAroundCenter:
        case PaintFormat_ScaleUniform:
        case PaintFormat_ScaleUniformAroundCenter: {
            bool uniform = base == PaintFormat_ScaleUniform || base == PaintFormat_ScaleUniformAroundCenter;
            bool centered = base == PaintFormat_ScaleAroundCenter || base == PaintFormat_ScaleUniformAroundCenter;
            uint32_t at = 4;
            float sx = paint.f2dot14(at);
            at += 2;
            float sy = sx;
            if (!uniform) {
                sy = paint.f2dot14(at);
                at += 2;
            }
            float cx = 0;
            float cy = 0;
            if (centered) {
                cx = float(paint.int16At(at));
                cy = float(paint.int16At(at + 2));
                at += 4;
            }
            if (variable) {
                uint32_t scales = uniform ? 1 : 2;
                deltasOf(w, paint.uint32At(at), scales + (centered ? 2 : 0), d);
                sx += d[0] / 16384;
                sy += d[uniform ? 0 : 1] / 16384;
                if (centered) {
                    cx += d[scales];
                    cy += d[scales + 1];
                }
            }
            pushTransform(w, aboutCenter(w, transformFrom(w, sx, 0, 0, sy, 0, 0), cx, cy), child(paint, 1), depth);
            break;
        }
        case PaintFormat_Rotate:
        case PaintFormat_RotateAroundCenter: {
            bool centered = base == PaintFormat_RotateAroundCenter;
            float angle = paint.f2dot14(4);
            float cx = centered ? float(paint.int16At(6)) : 0;
            float cy = centered ? float(paint.int16At(8)) : 0;
            if (variable) {
                deltasOf(w, paint.uint32At(centered ? 10 : 6), centered ? 3 : 1, d);
                angle += d[0] / 16384;
                cx += d[1];
                cy += d[2];
            }
            float radians = angle * kPiF32;
            float c = cosF32(radians);
            float s = sinF32(radians);
            pushTransform(w, aboutCenter(w, transformFrom(w, c, s, -s, c, 0, 0), cx, cy), child(paint, 1), depth);
            break;
        }
        case PaintFormat_Skew:
        case PaintFormat_SkewAroundCenter: {
            bool centered = base == PaintFormat_SkewAroundCenter;
            float xAngle = paint.f2dot14(4);
            float yAngle = paint.f2dot14(6);
            float cx = centered ? float(paint.int16At(8)) : 0;
            float cy = centered ? float(paint.int16At(10)) : 0;
            if (variable) {
                deltasOf(w, paint.uint32At(centered ? 12 : 8), centered ? 4 : 2, d);
                xAngle += d[0] / 16384;
                yAngle += d[1] / 16384;
                cx += d[2];
                cy += d[3];
            }
            float x = tanF32(-xAngle * kPiF32);
            float y = tanF32(yAngle * kPiF32);
            pushTransform(w, aboutCenter(w, transformFrom(w, 1, y, x, 1, 0, 0), cx, cy), child(paint, 1), depth);
            break;
        }
        case PaintFormat_Composite: {
            // The backdrop, then the source composited onto it, with the two alone in their own layer.
            uint8_t mode = paint.uint8At(4);
            ColrStep layer{};
            layer.op = ColrOp_PushLayer;
            layer.composite = PrFontComposite_SourceOver;
            w.walk.steps.push(layer);
            walkPaint(w, child(paint, 5), depth + 1);
            layer.composite = mode <= PrFontComposite_Luminosity ? PrFontComposite(mode) : PrFontComposite_SourceOver;
            w.walk.steps.push(layer);
            walkPaint(w, child(paint, 1), depth + 1);
            addStep(w, ColrOp_PopLayer);
            addStep(w, ColrOp_PopLayer);
            break;
        }
        default:
            break;
    }
}

// Finds the version 0 record of the glyph: its first layer and its number of layers. Returns false if it has none.
bool baseRecord(const ColrFace& face, uint32_t glyph, uint32_t* first, uint32_t* count) {
    const SfntTable& colr = face.colr;
    uint32_t records = colr.uint16At(2);
    uint32_t at = colr.uint32At(4);
    uint32_t i = partitionPoint(0, records, [&](uint32_t k) { return colr.uint16At(at + k * 6) < glyph; });
    if (i == records || colr.uint16At(at + i * 6) != glyph)
        return false;
    *first = colr.uint16At(at + i * 6 + 2);
    *count = colr.uint16At(at + i * 6 + 4);
    return *count != 0;
}

} // namespace

void colrOpen(const Sfnt& sfnt, ColrFace* out) {
    *out = ColrFace{};
    out->colr = sfntTable(sfnt, kColr);
    out->cpal = sfntTable(sfnt, kCpal);
    const SfntTable& colr = out->colr;
    if (colr.uint16At(0) >= 1 && colr.size >= 34) {
        uint32_t baseGlyphs = colr.uint32At(14);
        uint32_t layers = colr.uint32At(18);
        uint32_t map = colr.uint32At(26);
        uint32_t store = colr.uint32At(30);
        if (baseGlyphs != 0)
            out->baseGlyphs = colr.from(baseGlyphs);
        if (layers != 0)
            out->layers = colr.from(layers);
        if (map != 0)
            out->variationMap = colr.from(map);
        if (store != 0)
            out->variations = colr.from(store);
    }
}

bool colrHasGlyph(const ColrFace& face, uint32_t glyph) {
    uint32_t first = 0;
    uint32_t count = 0;
    return basePaint(face, glyph).size != 0 || baseRecord(face, glyph, &first, &count);
}

bool colrWalk(const ColrFace& face, SfntCoords coords, uint32_t glyph, ColrWalk* walk) {
    Walker w{face, coords, *walk, kPaintMaxVisits};
    SfntTable paint = basePaint(face, glyph);
    if (paint.size != 0) {
        walkPaint(w, paint, 0);
        return true;
    }
    // Version 0: each layer is a glyph filled with one color.
    uint32_t first = 0;
    uint32_t count = 0;
    if (!baseRecord(face, glyph, &first, &count))
        return false;
    const SfntTable& colr = face.colr;
    uint32_t layers = colr.uint32At(8);
    uint32_t layerCount = colr.uint16At(12);
    for (uint32_t i = 0; i < count && first + i < layerCount; ++i) {
        uint32_t record = layers + (first + i) * 4;
        ColrStep clip{};
        clip.op = ColrOp_PushClipGlyph;
        clip.glyph = colr.uint16At(record);
        walk->steps.push(clip);
        ColrStep fill{};
        fill.op = ColrOp_Fill;
        fill.paint.kind = PrFontPaintKind_Solid;
        fill.paint.color = colorOf(w, colr.uint16At(record + 2), 1);
        walk->steps.push(fill);
        addStep(w, ColrOp_PopClip);
    }
    return true;
}

uint32_t colrPaletteCount(const ColrFace& face) {
    return face.cpal.uint16At(4);
}

uint32_t colrPaletteEntryCount(const ColrFace& face) {
    return colrPaletteCount(face) != 0 ? face.cpal.uint16At(2) : 0;
}

PrColor colrPaletteEntry(const ColrFace& face, uint32_t palette, uint32_t index) {
    const SfntTable& cpal = face.cpal;
    uint32_t palettes = cpal.uint16At(4);
    uint32_t entries = cpal.uint16At(2);
    if (palettes == 0 || index >= entries)
        return PrColor{0, 0, 0, 0};
    if (palette >= palettes)
        palette = 0;
    uint32_t record = uint32_t(cpal.uint16At(12 + palette * 2)) + index;
    if (record >= cpal.uint16At(6))
        return PrColor{0, 0, 0, 0};
    uint32_t at = cpal.uint32At(8) + record * 4;
    return PrColor{float(cpal.uint8At(at + 2)) / 255, float(cpal.uint8At(at + 1)) / 255, float(cpal.uint8At(at)) / 255,
                   float(cpal.uint8At(at + 3)) / 255};
}

} // namespace pr_impl
