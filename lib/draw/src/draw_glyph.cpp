// Glyphs as a backend draws them.

#include "pr/draw_glyph.h"
#include "draw/src/draw_glyph.h"

#include "pr/alloc.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// Color glyphs

// The recording target of a color glyph's painter: the list, and a clip to a glyph whose push is deferred. A fill
// directly inside that clip, and not nested deeper, fills the glyph's outline instead of pushing the clip. This is the
// common case. Anything pushed inside the clip pushes the clip first.
struct GlyphPainter {
    const PrFont* font;
    PrDrawList* list;
    PrPath* outline;
    RawArray<uint16_t> clips;              // the glyph of each open clip
    RawArray<bool> pushed;                 // true for each open clip that was pushed
    RawArray<PrAffine2> transforms;        // each open transform, composed with the transforms outside it, to ems
    RawArray<PrDrawColorStop> stops;
    RawArray<PrDrawColorStop> conic;            // a conic gradient's stops over the whole turn
};

// The maximum number of stops that a conic gradient's repetitions may use over the full turn.
constexpr uint32_t kConicMaxStops = 1024;

// Returns the parameter in [0, 1] that the extend mode maps `t` to.
float extendOf(PrFontExtend extend, float t) {
    if (extend == PrFontExtend_Repeat)
        return t - prF32Floor(t);
    if (extend == PrFontExtend_Reflect) {
        float m = t - 2 * prF32Floor(t / 2);
        return m > 1 ? 2 - m : m;
    }
    return prF32Clamp(t, 0, 1);
}

// Returns the color at `t` of stops fitted to [0, 1], mixed premultiplied.
PrColor stopColorAt(const RawArray<PrDrawColorStop>& stops, float t) {
    uint32_t i = 0;
    while (i + 1 < stops.count && stops.data[i + 1].offset <= t)
        ++i;
    if (i + 1 >= stops.count || t <= stops.data[i].offset)
        return stops.data[i].color;
    PrColor a = stops.data[i].color;
    PrColor b = stops.data[i + 1].color;
    float f = (t - stops.data[i].offset) / (stops.data[i + 1].offset - stops.data[i].offset);
    float alpha = a.a + (b.a - a.a) * f;
    PrColor out{0, 0, 0, alpha};
    if (alpha > 0) {
        auto mix = [&](float x, float y) {
            return (x * a.a + (y * b.a - x * a.a) * f) / alpha;
        };
        out.r = mix(a.r, b.r);
        out.g = mix(a.g, b.g);
        out.b = mix(a.b, b.b);
    }
    return out;
}

void pushPendingClip(GlyphPainter& p) {
    if (p.pushed.count == 0 || p.pushed.last())
        return;
    prPathClear(p.outline);
    prFontGlyphOutline(p.font, p.clips.last(), 0, prAffine2Identity(), p.outline);
    prDrawListPushClipPath(p.list, p.outline, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    p.pushed.last() = true;
}

void glyphPushTransform(void* PR_NULLABLE context, PrAffine2 transform) {
    GlyphPainter& p = *static_cast<GlyphPainter*>(context);
    pushPendingClip(p);
    prDrawListPushTransformAffine2(p.list, transform);
    p.transforms.push(p.transforms.count != 0 ? prAffine2Concat(transform, p.transforms.last()) : transform);
}

void glyphPopTransform(void* PR_NULLABLE context) {
    GlyphPainter& p = *static_cast<GlyphPainter*>(context);
    prDrawListPopTransform(p.list);
    p.transforms.pop();
}

void glyphPushClip(void* PR_NULLABLE context, uint16_t glyph) {
    GlyphPainter& p = *static_cast<GlyphPainter*>(context);
    pushPendingClip(p);
    p.clips.push(glyph);
    p.pushed.push(false);
}

void glyphPopClip(void* PR_NULLABLE context) {
    GlyphPainter& p = *static_cast<GlyphPainter*>(context);
    p.clips.pop();
    bool pushed = p.pushed.last();
    p.pushed.pop();
    if (pushed)
        prDrawListPopClip(p.list);
}

void glyphPushLayer(void* PR_NULLABLE context, PrFontComposite composite) {
    GlyphPainter& p = *static_cast<GlyphPainter*>(context);
    pushPendingClip(p);
    PrDrawGroup group = prDrawGroupDefault();
    group.isolated = true;
    switch (composite) {
        case PrFontComposite_Clear:
            group.composite = PrDrawComposite_Clear;
            break;
        case PrFontComposite_Source:
            group.composite = PrDrawComposite_Copy;
            break;
        case PrFontComposite_Destination:
            group.opacity = 0;
            break;
        case PrFontComposite_DestinationOver:
            group.composite = PrDrawComposite_DestinationOver;
            break;
        case PrFontComposite_SourceIn:
            group.composite = PrDrawComposite_SourceIn;
            break;
        case PrFontComposite_DestinationIn:
            group.composite = PrDrawComposite_DestinationIn;
            break;
        case PrFontComposite_SourceOut:
            group.composite = PrDrawComposite_SourceOut;
            break;
        case PrFontComposite_DestinationOut:
            group.composite = PrDrawComposite_DestinationOut;
            break;
        case PrFontComposite_SourceAtop:
            group.composite = PrDrawComposite_SourceAtop;
            break;
        case PrFontComposite_DestinationAtop:
            group.composite = PrDrawComposite_DestinationAtop;
            break;
        case PrFontComposite_Xor:
            group.composite = PrDrawComposite_Xor;
            break;
        case PrFontComposite_Plus:
            group.composite = PrDrawComposite_PlusLighter;
            break;
        default:
            if (composite >= PrFontComposite_Screen) {
                // The blend modes, in the order COLR lists them.
                constexpr PrDrawBlendMode kBlend[] = {
                    PrDrawBlendMode_Screen,     PrDrawBlendMode_Overlay,   PrDrawBlendMode_Darken,
                    PrDrawBlendMode_Lighten,    PrDrawBlendMode_ColorDodge, PrDrawBlendMode_ColorBurn,
                    PrDrawBlendMode_HardLight,  PrDrawBlendMode_SoftLight, PrDrawBlendMode_Difference,
                    PrDrawBlendMode_Exclusion,  PrDrawBlendMode_Multiply,  PrDrawBlendMode_Hue,
                    PrDrawBlendMode_Saturation, PrDrawBlendMode_Color,     PrDrawBlendMode_Luminosity,
                };
                uint32_t index = uint32_t(composite - PrFontComposite_Screen);
                if (index < countOf(kBlend))
                    group.blendMode = kBlend[index];
            }
            break;
    }
    prDrawListPushGroup(p.list, &group);
}

void glyphPopLayer(void* PR_NULLABLE context) {
    prDrawListPopGroup(static_cast<GlyphPainter*>(context)->list);
}

static_assert(uint32_t(PrDrawExtend_Pad) == PrFontExtend_Pad && uint32_t(PrDrawExtend_Repeat) == PrFontExtend_Repeat &&
    uint32_t(PrDrawExtend_Reflect) == PrFontExtend_Reflect);

// Converts the paint to a brush, with its stops fitted to [0, 1] and its geometry adjusted to match. Returns false for
// a paint with no stops.
bool brushOf(GlyphPainter& p, const PrFontPaint& paint, PrDrawBrush* brush) {
    if (paint.stopCount == 0)
        return false;
    float first = paint.stops[0].offset;
    float last = paint.stops[paint.stopCount - 1].offset;
    float span = last - first;
    if (!(span > 0)) {
        first = 0;
        last = 1;
        span = 1;
    }
    p.stops.clear();
    for (uint32_t i = 0; i < paint.stopCount; ++i) {
        const PrFontColorStop& stop = paint.stops[i];
        p.stops.push(PrDrawColorStop{.offset = (stop.offset - first) / span, .color = stop.color});
    }
    switch (paint.kind) {
        case PrFontPaintKind_Linear: {
            // The gradient runs from start to end, projected onto the normal of the line from start to the rotation
            // point.
            PrVec2 p0 = paint.linear.start;
            PrVec2 p1 = paint.linear.end;
            PrVec2 q2{paint.linear.rotationPoint.x - p0.x, paint.linear.rotationPoint.y - p0.y};
            PrVec2 q1{p1.x - p0.x, p1.y - p0.y};
            float length = q2.x * q2.x + q2.y * q2.y;
            if (length > 1e-12f) {
                float k = (q2.x * q1.x + q2.y * q1.y) / length;
                p1 = p1 - k * q2;
            }
            *brush =
                prDrawBrushLinear(prVec2Lerp(p0, p1, first), prVec2Lerp(p0, p1, last), p.stops.data, p.stops.count);
            break;
        }
        case PrFontPaintKind_Radial: {
            float growth = paint.radial.endRadius - paint.radial.startRadius;
            *brush = prDrawBrushRadial(prVec2Lerp(paint.radial.start, paint.radial.end, first),
                prF32Max(paint.radial.startRadius + growth * first, 0),
                prVec2Lerp(paint.radial.start, paint.radial.end, last), prF32Max(paint.radial.startRadius + growth * last,
                    0), p.stops.data, p.stops.count);
            break;
        }
        case PrFontPaintKind_Conic: {
            // COLR's parameter is (angle - start) / (end - start), extended past the stops. The angle is
            // counterclockwise with y up, from +x, in [0, 2pi). The parameter is piecewise linear in the angle. Stops
            // at its breaks over the whole turn therefore reproduce it exactly with a conic brush, mirrored to turn the
            // same way.
            float start = -paint.conic.startAngle;
            float end = -paint.conic.endAngle;
            float s0 = start + (end - start) * first;
            float span1 = (end - start) * last - (end - start) * first;
            if (!(span1 != 0))
                return false;
            float t0 = -s0 / span1;
            float t1 = (2 * kPrF32Pi - s0) / span1;
            float low = prF32Min(t0, t1);
            float high = prF32Max(t0, t1);
            p.conic.clear();
            p.conic.push(PrDrawColorStop{.offset = low, .color = stopColorAt(p.stops, extendOf(paint.extend, low))});
            int32_t from = paint.extend == PrFontExtend_Pad ? 0 : int32_t(prF32Floor(low));
            int32_t to = paint.extend == PrFontExtend_Pad ? 0 : int32_t(prF32Floor(high));
            for (int32_t k = from; k <= to && p.conic.count < kConicMaxStops; ++k) {
                bool reflected = paint.extend == PrFontExtend_Reflect && (k & 1) != 0;
                for (uint32_t i = 0; i < p.stops.count; ++i) {
                    const PrDrawColorStop& stop = p.stops.data[reflected ? p.stops.count - 1 - i : i];
                    float t = reflected ? float(k) + 1 - stop.offset : float(k) + stop.offset;
                    if (t > low && t < high)
                        p.conic.push(PrDrawColorStop{.offset = t, .color = stop.color});
                }
            }
            p.conic.push(PrDrawColorStop{.offset = high, .color = stopColorAt(p.stops, extendOf(paint.extend, high))});
            for (uint32_t i = 0; i < p.conic.count; ++i)
                p.conic.data[i].offset = (s0 + p.conic.data[i].offset * span1) / (2 * kPrF32Pi);
            if (span1 < 0) {
                for (uint32_t i = 0, j = p.conic.count - 1; i < j; ++i, --j) {
                    PrDrawColorStop swap = p.conic.data[i];
                    p.conic.data[i] = p.conic.data[j];
                    p.conic.data[j] = swap;
                }
            }
            PrVec2 c = paint.conic.center;
            *brush = prDrawBrushConic(c, 0, p.conic.data, p.conic.count);
            brush->extend = PrDrawExtend(paint.extend);
            brush->transform = prAffine2Concat(prAffine2Concat(prAffine2Translate(-c), prAffine2Scale(PrVec2{1, -1})),
                prAffine2Translate(c));
            return true;
        }
        default:
            return false;
    }
    brush->extend = PrDrawExtend(paint.extend);
    return true;
}

void glyphFill(void* PR_NULLABLE context, const PrFontPaint* paint) {
    GlyphPainter& p = *static_cast<GlyphPainter*>(context);
    PrDrawPaint fill{.color = paint->color};
    PrDrawBrush brush{};
    if (paint->kind != PrFontPaintKind_Solid) {
        if (!brushOf(p, *paint, &brush))
            return;
        fill.brush = &brush;
    }
    if (p.pushed.count != 0 && !p.pushed.last()) {
        prPathClear(p.outline);
        prFontGlyphOutline(p.font, p.clips.last(), 0, prAffine2Identity(), p.outline);
        prDrawListPath(p.list, p.outline, PrPathFillRule_NonZero, fill);
    } else {
        // Covers everything that the clips let through. A glyph's ems never extend past this rectangle, which the
        // transforms pushed since the clip map from ems.
        PrRect ems{-16, -16, 16, 16};
        PrAffine2 fromEms = prAffine2Identity();
        if (p.transforms.count == 0 || prAffine2Invert(p.transforms.last(), &fromEms))
            prDrawListRect(p.list, prAffine2MapRect(fromEms, ems), fill);
    }
}

// Glyph lists

// The inputs that a glyph's list is drawn from, compared by their bytes. Color layers and SVG documents are keyed on
// their colors, and an image is keyed on its strike.
PR_NO_PADDING_BEGIN
struct GlyphKey {
    uint64_t font;
    uint16_t glyph;
    uint16_t _pad0;
    uint32_t palette;
    uint64_t colors;                        // a hash of the palette's color overrides
    PrColor foreground;
    float strike;                           // an image's strike; zero for color layers and SVG documents
    uint32_t _pad;
};
PR_NO_PADDING_END

struct GlyphEntry {
    GlyphKey key;
    PrDrawList* PR_NULLABLE list;           // null for a glyph that does not paint itself
    uint64_t lastUsed;
};

struct DrawGlyphCache {
    RawArray<GlyphEntry> entries;
    HashIndex index;
    uint64_t use;
    PrDrawGlyphDecoder* PR_NULLABLE decoder;
};

uint64_t colorsHash(const PrFontPalette* PR_NULLABLE palette) {
    if (palette == nullptr || palette->colorCount == 0)
        return 0;
    return prHashBytes(palette->colors, sizeof(PrFontPaletteColor) * palette->colorCount, 1);
}

uint32_t hashOf(const GlyphKey& key) {
    return uint32_t(prHashBytes(&key, sizeof(key), 0));
}

// The decoder of the images of bitmap strikes (prDrawGlyphBitmapCreate).
struct BitmapDecoder : PrDrawGlyphDecoder {
    PrImageCodec* codec;
};

void destroyBitmapDecoder(PrDrawGlyphDecoder* decoder) {
    prMemoryFree(decoder);
}

bool decodeBitmap(PrDrawGlyphDecoder* decoder, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        const PrFontPalette* PR_NULLABLE, PrColor, PrDrawList* list) {
    PrFontGlyphSvg svg{};
    if (prFontGlyphSvg(font, glyph, &svg))
        return false;
    BitmapDecoder* bitmap = static_cast<BitmapDecoder*>(decoder);
    return prDrawGlyphImage(bitmap->codec, font, glyph, pixelsPerEm, list);
}

} // namespace

PrDrawList* PR_NULLABLE drawGlyphList(const PrFont* font, uint16_t glyph, const PrFontPalette* PR_NULLABLE palette,
        PrColor foreground) {
    GlyphPainter p{};
    p.font = font;
    p.list = prDrawListCreate();
    p.outline = prPathCreate();
    PrFontPainter painter{
        &p, glyphPushTransform, glyphPopTransform, glyphPushClip, glyphPopClip,
        glyphPushLayer, glyphPopLayer, glyphFill,
    };
    bool painted = prFontGlyphPaint(font, glyph, palette, foreground, &painter) &&
        prDrawListCommandCount(p.list) != 0 && prDrawListDepth(p.list) == 0;
    prPathDestroy(p.outline);
    p.clips.reset();
    p.pushed.reset();
    p.transforms.reset();
    p.stops.reset();
    p.conic.reset();
    if (!painted) {
        prDrawListRelease(p.list);
        return nullptr;
    }
    return p.list;
}

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDrawGlyphCache : DrawGlyphCache {};

PrAffine2 prDrawGlyphTransform(const PrFontGlyphRun* run, uint32_t index) {
    float oblique = prFontOblique(run->font);
    PrAffine2 t{run->size, 0, -oblique * run->size, run->size, 0, 0};
    if (run->transforms != nullptr)
        t = prAffine2Concat(t, run->transforms[index]);
    PrVec2 at = run->glyphs[index].position;
    return prAffine2Concat(t, prAffine2Translate(at));
}

PrDrawGlyphPlacement prDrawGlyphPlace(const PrFontGlyphRun* run, uint32_t index, PrAffine2 toTarget) {
    PrDrawGlyphPlacement out{};
    out.glyph = prAffine2Concat(prDrawGlyphTransform(run, index), toTarget);
    out.origin = prAffine2MapPoint(toTarget, run->glyphs[index].position);
    const PrAffine2& g = out.glyph;
    float em = prF32Max(prF32Abs(g.a), prF32Abs(g.d));
    out.phased = g.b == 0 && g.c == 0 && g.a > 0 && g.d > 0 && em <= kPrDrawGlyphMaxPhasedEm &&
        prF32IsFinite(out.origin.x) && prF32IsFinite(out.origin.y);
    if (out.phased) {
        float column = prF32Floor(out.origin.x);
        float row = prF32Floor(out.origin.y);
        float phases = float(kPrDrawGlyphPhases);
        out.origin = PrVec2{
            column + prF32Round((out.origin.x - column) * phases) / phases,
            row + prF32Round((out.origin.y - row) * phases) / phases,
        };
    }
    return out;
}

bool prDrawGlyphPath(const PrFontGlyphRun* run, uint32_t index, const PrAffine2* PR_NULLABLE toTarget, PrPath* out) {
    PrAffine2 t = prDrawGlyphTransform(run, index);
    const PrFont* font = run->font;
    float pixelsPerEm = 0;
    if (prFontHinting(font) != PrFontHinting_None && toTarget != nullptr) {
        // Hinting fits an em only if its transform to the target is a plain scale.
        PrAffine2 whole = prAffine2Concat(t, *toTarget);
        if (whole.b == 0 && whole.c == 0 && whole.a > 0 && prF32Abs(whole.a - whole.d) < 1e-4f * whole.a)
            pixelsPerEm = whole.a;
    }
    return prFontGlyphOutline(font, run->glyphs[index].id, pixelsPerEm, t, out);
}

float prDrawGlyphPixelsPerEm(const PrFontGlyphRun* run, uint32_t index, PrAffine2 toTarget) {
    return prAffine2MaxScale(prAffine2Concat(prDrawGlyphTransform(run, index), toTarget));
}

// ---------------------------------------------------------------------------------------------------------------------
// Glyph decoders

void prDrawGlyphDecoderDestroy(PrDrawGlyphDecoder* PR_NULLABLE decoder) {
    if (decoder != nullptr && decoder->destroy != nullptr)
        decoder->destroy(decoder);
}

bool prDrawGlyphDecoderDecode(PrDrawGlyphDecoder* decoder, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        const PrFontPalette* PR_NULLABLE palette, PrColor foreground, PrDrawList* list) {
    return decoder->decode(decoder, font, glyph, pixelsPerEm, palette, foreground, list);
}

bool prDrawGlyphImage(PrImageCodec* codec, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrDrawList* list) {
    PrFontGlyphImage image{};
    const PrDrawImage* decoded = nullptr;
    if (!prFontGlyphImage(font, glyph, pixelsPerEm, &image) ||
            prDrawImageCreateFromBytes(codec, image.bytes, &decoded, nullptr) != PrStatus_Ok)
        return false;
    PrRect whole{0, 0, float(prDrawImageExtent(decoded).x), float(prDrawImageExtent(decoded).y)};
    PrDrawSource drawn = prDrawSourceImage(decoded, whole);
    PrDrawBrush brush = prDrawBrushImage(drawn, image.bounds, PrDrawSampling_Mipmap);
    prDrawListRect(list, image.bounds, {.brush = &brush});
    prDrawImageRelease(decoded);
    return true;
}

PrDrawGlyphDecoder* prDrawGlyphBitmapCreate(PrImageCodec* codec) {
    BitmapDecoder* decoder = allocZeroed<BitmapDecoder>();
    decoder->destroy = destroyBitmapDecoder;
    decoder->decode = decodeBitmap;
    decoder->codec = codec;
    return decoder;
}

// ---------------------------------------------------------------------------------------------------------------------
// Glyph cache

PrDrawGlyphCache* prDrawGlyphCacheCreate(PrDrawGlyphDecoder* PR_NULLABLE decoder) {
    PrDrawGlyphCache* cache = allocZeroed<PrDrawGlyphCache>();
    cache->decoder = decoder;
    return cache;
}

void prDrawGlyphCacheDestroy(PrDrawGlyphCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    for (uint32_t i = 0; i < cache->entries.count; ++i)
        prDrawListRelease(cache->entries.data[i].list);
    cache->entries.reset();
    cache->index.reset();
    prMemoryFree(cache);
}

void prDrawGlyphCacheBegin(PrDrawGlyphCache* cache) {
    ++cache->use;
}

void prDrawGlyphCacheEnd(PrDrawGlyphCache* cache) {
    uint32_t kept = 0;
    cache->index.clear();
    for (uint32_t i = 0; i < cache->entries.count; ++i) {
        GlyphEntry& e = cache->entries.data[i];
        if (e.lastUsed != cache->use) {
            prDrawListRelease(e.list);
            continue;
        }
        cache->entries.data[kept] = e;
        cache->index.insert(hashOf(e.key), kept);
        ++kept;
    }
    cache->entries.count = kept;
}

PrDrawList* PR_NULLABLE prDrawGlyphCacheList(PrDrawGlyphCache* cache, const PrFont* font, uint16_t glyph,
        float pixelsPerEm, const PrFontPalette* PR_NULLABLE palette, PrColor foreground) {
    GlyphKey key{};
    key.font = prFontId(font);
    key.glyph = glyph;
    PrFontGlyphSvg svg{};
    PrFontGlyphImage image{};
    bool layered = prFontGlyphHasColor(font, glyph);
    if (layered || (cache->decoder != nullptr && prFontGlyphSvg(font, glyph, &svg))) {
        key.palette = palette != nullptr ? palette->index : 0;
        key.colors = colorsHash(palette);
        key.foreground = foreground;
    } else if (cache->decoder != nullptr && prFontGlyphImage(font, glyph, pixelsPerEm, &image)) {
        key.strike = image.pixelsPerEm;
    } else {
        return nullptr;
    }
    uint32_t hash = hashOf(key);
    uint32_t found = cache->index.find(
        hash, [&](uint32_t i) { return prMemEquals(&cache->entries.data[i].key, &key, sizeof(key)); });
    if (found != kHashMissing) {
        cache->entries.data[found].lastUsed = cache->use;
        return cache->entries.data[found].list;
    }
    PrDrawList* list = nullptr;
    if (layered) {
        list = drawGlyphList(font, glyph, palette, foreground);
    } else {
        list = prDrawListCreate();
        if (!prDrawGlyphDecoderDecode(cache->decoder, font, glyph, pixelsPerEm, palette, foreground, list)) {
            prDrawListRelease(list);
            list = nullptr;
        }
    }
    cache->index.insert(hash, cache->entries.count);
    cache->entries.push(GlyphEntry{key, list, cache->use});
    return list;
}

uint32_t prDrawGlyphListCommands(const PrDrawCommand* glyphs, uint32_t index, PrDrawList* glyphList, PrDrawGroup* group,
        PrDrawCommand out[PR_NONNULL kPrDrawGlyphMaxCommands]) {
    *group = PrDrawGroup{};
    group->opacity = prF32Clamp(prFloat16ToFloat(glyphs->color.a), 0, 1);
    group->composite = glyphs->composite;
    group->blendMode = glyphs->blendMode;
    prMemZero(out, sizeof(PrDrawCommand) * 5);
    uint32_t count = 0;
    out[count].kind = PrDrawCommandKind_PushTransformAffine2;
    out[count++].affine = prDAffine2FromAffine2(prDrawGlyphTransform(glyphs->glyphs, index));
    bool grouped = prDrawGroupHasEffect(group);
    if (grouped) {
        out[count].kind = PrDrawCommandKind_PushGroup;
        out[count].composite = glyphs->composite;
        out[count].blendMode = glyphs->blendMode;
        out[count++].group = group;
    }
    out[count].kind = PrDrawCommandKind_Embed;
    out[count++].embed.list = glyphList;
    if (grouped)
        out[count++].kind = PrDrawCommandKind_PopGroup;
    out[count++].kind = PrDrawCommandKind_PopTransform;
    return count;
}
