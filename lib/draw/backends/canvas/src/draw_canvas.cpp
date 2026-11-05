// The Canvas 2D draw backend. As a pass's work runs, the backend walks a frame's list and makes calls on the 2D context
// of the canvas that is the pass's attachment. The page's side of the backend (draw_canvas.js) makes those calls.
//
// The browser rasterizes paths, strokes, gradients, patterns, images and shadows. The walk lowers what the context
// cannot express before it calls the page. Borders, decorations and strokes that the context does not stroke become
// fills. A ramp that does not mix as the context mixes becomes stops that lie close enough together. A brush that the
// context has no paint for becomes texels. A group with an effect draws into a canvas of its own, which then composites
// onto what lies beneath it.

#include "pr/draw_canvas.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/draw_image.h"
#include "pr/draw_lower.h"
#include "pr/draw_transform.h"
#include "pr/float.h"
#include "pr/hash_index.h"
#include "pr/memory.h"
#include "pr/path_polyline.h"
#include "pr/raw_array.h"

// The page's side of the backend (draw_canvas.js). The page keeps a renderer's caches between plays. A play draws into
// one canvas between Begin and End. Each call draws on the play's top surface: the canvas itself, or a layer, mask or
// list that the play pushed. Coordinates are in target pixels, and the page places them at the render's origin on the
// canvas. Colors are straight sRGB in [0, 1].
//
// A path is passed as its verbs (PrPathVerb) and points, or as a polyline's points, contours and closed bits. A source
// is eight words: a kind (CanvasSource) followed by its operands. Each stop is five floats: offset, r, g, b and a. Each
// filter is floats: a kind (CanvasFilter) followed by its operands.
extern "C" uint32_t prDrawCanvasOpen(void);
extern "C" void prDrawCanvasClose(uint32_t renderer);
extern "C" void prDrawCanvasBegin(uint32_t renderer, PrGpuCanvas canvas, int32_t originX, int32_t originY);
extern "C" void prDrawCanvasEnd(void);
extern "C" void prDrawCanvasSave(void);
extern "C" void prDrawCanvasRestore(void);
extern "C" void prDrawCanvasTransform(float a, float b, float c, float d, float e, float f);
extern "C" void prDrawCanvasPath(const PrPathVerb* verbs, uint32_t verbCount, const PrVec2* points);
extern "C" void prDrawCanvasPolyline(const PrVec2* points, uint32_t pointCount, const uint32_t* contours,
    uint32_t contourCount, const uint64_t* closed);
extern "C" void prDrawCanvasRect(float left, float top, float right, float bottom);
extern "C" void prDrawCanvasGlyph(uint32_t id, const PrPathVerb* verbs, uint32_t verbCount, const PrVec2* points);
extern "C" void prDrawCanvasMapPath(float a, float b, float c, float d, float e, float f);
extern "C" void prDrawCanvasColor(float r, float g, float b, float a);
extern "C" void prDrawCanvasLinear(float x0, float y0, float x1, float y1, const float* stops, uint32_t count);
extern "C" void prDrawCanvasRadial(float x0, float y0, float r0, float x1, float y1, float r1, const float* stops,
    uint32_t count);
extern "C" void prDrawCanvasConic(float angle, float x, float y, const float* stops, uint32_t count);
extern "C" void prDrawCanvasPattern(const uint32_t* source, float cropX, float cropY, float cropWidth,
    float cropHeight, uint32_t extendX, uint32_t extendY, const uint32_t* outset, float a, float b, float c, float d, float e,
    float f, uint32_t sampling);
extern "C" void prDrawCanvasComposite(uint32_t operation);
extern "C" void prDrawCanvasAlpha(float alpha);
extern "C" void prDrawCanvasFill(uint32_t rule);
extern "C" void prDrawCanvasStroke(float width, uint32_t cap, uint32_t join, float miterLimit, float dashOffset,
    const float* dashes, uint32_t dashCount);
extern "C" void prDrawCanvasClip(uint32_t rule);
extern "C" void prDrawCanvasClear(float r, float g, float b, float a);
extern "C" void prDrawCanvasShadowFill(float shift, float sigma, float r, float g, float b, float a, uint32_t rule);
extern "C" void prDrawCanvasPushLayer(int32_t left, int32_t top, uint32_t width, uint32_t height);
extern "C" void prDrawCanvasPopLayer(float alpha, uint32_t operation, const float* filters, uint32_t count);
extern "C" void prDrawCanvasBackdrop(const float* filters, uint32_t count);
extern "C" void prDrawCanvasPushMask(void);
extern "C" void prDrawCanvasPopMask(bool luminance, bool invert);
extern "C" void prDrawCanvasPushList(uint32_t width, uint32_t height);
extern "C" uint32_t prDrawCanvasPopList(void);
extern "C" void prDrawCanvasDrawImage(const uint32_t* source, float sx, float sy, float sw, float sh, float dx,
    float dy, float dw, float dh, uint32_t sampling);
extern "C" void prDrawCanvasPutPixels(const uint8_t* texels, size_t stride, uint32_t width, uint32_t height,
    int32_t x, int32_t y, uint32_t flags);

namespace pr_impl {

namespace {

// The kinds of source, numbered as the page reads them. Each comment lists the source's operands.
enum {
    // id (two words, low first), pointer, stride, width, height and flags: an image's texels, which the page caches by
    // the image's id
    CanvasSource_Image,
    CanvasSource_Texture,   // the canvas of a texture (PrGpuCanvas)
    CanvasSource_List,      // the index of a list that the play drew (prDrawCanvasPopList)
    CanvasSource_Pixels,    // pointer, stride, width, height and flags: texels that only this call uses
};
typedef uint32_t CanvasSource;

// How 8-bit texels are laid out, and what their alpha means.
enum {
    CanvasPixels_Bgra = 1u << 0,        // BGRA, not RGBA
    CanvasPixels_Straight = 1u << 1,    // straight alpha, not premultiplied
    CanvasPixels_Opaque = 1u << 2,      // every texel is opaque, whatever its alpha holds
};
typedef uint32_t CanvasPixels;

// The kinds of filter, numbered as the page reads them.
enum {
    CanvasFilter_Blur,          // sigma, in target pixels
    CanvasFilter_DropShadow,    // dx dy sigma r g b a, in target pixels
};
typedef uint32_t CanvasFilter;

// How an image is sampled: without smoothing, or with the browser's low or high quality.
enum {
    CanvasSampling_Nearest,
    CanvasSampling_Low,
    CanvasSampling_High,
};
typedef uint32_t CanvasSampling;

// The maximum distance of a flattened curve, or of a lowered stroke's round parts, from the true shape, in target
// pixels.
constexpr float kTolerance = 0.2f;

// The number of colors per unit of the gradient's parameter in a ramp's pieces that mix nonlinearly, and the number of
// stops between the ends of a piece whose alpha changes. The context interpolates stops unpremultiplied, so such a
// piece needs stops close together.
constexpr uint32_t kRampResolution = 32;
constexpr uint32_t kAlphaSteps = 8;

// The most periods of a repeating gradient that the context draws as one gradient. A gradient with more is drawn as
// texels.
constexpr float kMaxPeriods = 256;

// The largest side, in pixels, of the surface that a list source is drawn into.
constexpr uint32_t kListMaxSize = 4096;

// A glyph that no play has drawn for this many plays is dropped.
constexpr uint32_t kGlyphAge = 240;

// A glyph's outline in ems, cached across plays. The page caches the outline's path by `id`, which no other outline
// reuses.
struct CanvasGlyph {
    uint64_t key;
    uint32_t id;
    uint32_t used;                  // the serial of the last play that drew the glyph
    PrPath* PR_NULLABLE path;       // null for a glyph with no outline
};

// The renderer: its caches, and the scratch storage of its plays.
struct CanvasBackend : PrDrawRenderer {
    uint32_t pageState;                  // the page's caches for this renderer (prDrawCanvasOpen)
    PrDrawGlyphCache* glyphCache;
    HashIndex glyphIndex;           // the glyphs' slots, by font and glyph
    RawArray<CanvasGlyph> glyphs;   // a free slot has no id
    RawArray<uint32_t> freeGlyphs;
    uint32_t nextGlyphId;
    uint32_t serial;

    RawArray<float> operands;         // stops and filters for the call being made
    RawArray<uint8_t> scratchTexels;  // texels for the call being made (CanvasSource_Pixels, or an image's copy)
    PrPath* path;
    PrPath* extra;
    PrPathPolyline fill;
    PrPathPolyline aligned;
    PrPathPolyline region;
};

// ---------------------------------------------------------------------------------------------------------------------
// Calls

void sendPath(const PrPath* path) {
    prDrawCanvasPath(prPathVerbs(path), prPathVerbCount(path), prPathPoints(path));
}

void sendPolyline(const PrPathPolyline* p) {
    prDrawCanvasPolyline(p->points, p->pointCount, p->contours, p->contourCount, p->closed);
}

void sendTransform(PrAffine2 m) {
    prDrawCanvasTransform(m.a, m.b, m.c, m.d, m.e, m.f);
}

void sendMapPath(PrAffine2 m) {
    if (!prAffine2IsIdentity(m))
        prDrawCanvasMapPath(m.a, m.b, m.c, m.d, m.e, m.f);
}

void sendColor(PrColor c) {
    prDrawCanvasColor(prF32Clamp(c.r, 0, 1), prF32Clamp(c.g, 0, 1), prF32Clamp(c.b, 0, 1), prF32Clamp(c.a, 0, 1));
}

uint32_t canvasRule(PrPathFillRule rule) {
    return rule == PrPathFillRule_EvenOdd ? 1 : 0;
}

// Returns the index of the page's name for a draw's composite operation. The context composites a blend mode
// source-over. It has no operation for HardMix or PlusDarker, so each draws as the nearest operation that it has.
uint32_t canvasOperation(PrDrawComposite composite, PrDrawBlendMode blend) {
    if (blend != PrDrawBlendMode_Normal)
        return PrDrawComposite_Count + blend;
    return composite;
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

// A pushed group: whether it pushed a layer, and what its pop composites.
struct Group {
    bool layered;
    const PrDrawGroup* PR_NULLABLE group;
    PrDrawComposite composite;
    PrDrawBlendMode blendMode;
    PrDrawTransform transform;      // the transform at the push
    PrIRect box;                    // the layer's box, in target pixels
    uint32_t clipDepth;
};

// A list source that this play drew: the list, its area, the size that it was drawn at, and its index.
struct DrawnList {
    uint64_t id;
    PrRect area;
    PrUVec2 size;
    uint32_t index;
};

// The state of a walk over one surface: the render's, or that of a list drawn for a source. `operation` and `alpha` are
// the values that the surface's context holds. After a restore or a push makes them unknown, they are ~0 and a negative
// alpha.
struct Walk {
    CanvasBackend* backend;
    RawArray<PrDrawTransform> transforms;
    RawArray<PrIRect> clips;        // the boxes of target pixels that the clips let through
    RawArray<Group> groups;
    RawArray<DrawnList>* drawn;     // the lists that the play drew, which all of its walks share
    uint32_t operation;
    float alpha;
};

void runCommands(Walk& w, const PrDrawCommand* commands, uint32_t count);

void forgetState(Walk& w) {
    w.operation = ~0u;
    w.alpha = -1;
}

void restore(Walk& w) {
    prDrawCanvasRestore();
    forgetState(w);
}

void setOperation(Walk& w, PrDrawComposite composite, PrDrawBlendMode blend) {
    uint32_t operation = canvasOperation(composite, blend);
    if (operation == w.operation)
        return;
    prDrawCanvasComposite(operation);
    w.operation = operation;
}

void setAlpha(Walk& w, float alpha) {
    if (alpha == w.alpha)
        return;
    prDrawCanvasAlpha(alpha);
    w.alpha = alpha;
}

const PrDrawTransform& current(const Walk& w) {
    return w.transforms.last();
}

PrIRect clipBox(const Walk& w) {
    return w.clips.last();
}

PrIRect deviceBox(PrAffine2 m, PrRect r) {
    PrRect mapped = prAffine2MapRect(m, r);
    constexpr float kLimit = float(kPrIRectMaxCoordinate);
    mapped.left = prF32Clamp(mapped.left, -kLimit, kLimit);
    mapped.top = prF32Clamp(mapped.top, -kLimit, kLimit);
    mapped.right = prF32Clamp(mapped.right, -kLimit, kLimit);
    mapped.bottom = prF32Clamp(mapped.bottom, -kLimit, kLimit);
    return prIRectRoundOut(mapped);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sources

// A source resolved for a call: its words, the rect of the texels that the source covers, and the extent of all of its
// texels.
struct Source {
    uint32_t words[8];
    PrRect texels;
    PrUVec2 extent;
};

// Returns `size` bytes of scratch storage for the texels of the call being made.
uint8_t* scratchTexelsFor(CanvasBackend* backend, size_t size) {
    backend->scratchTexels.resize(uint32_t(size));
    return backend->scratchTexels.data;
}

// Resolves an image to its own texels if they have 8 bits a channel, or else to a copy of them in Rgba8.
void imageSource(CanvasBackend* backend, const PrDrawImage* image, Source* out) {
    PrPixelView pixels = prDrawImagePixels(image);
    PrPixelFormat format = prDrawImageFormat(image);
    PrDrawAlpha alpha = prDrawImageAlpha(image);
    uint64_t id = prDrawImageId(image);
    CanvasPixels flags = 0;
    if (alpha == PrDrawAlpha_Straight)
        flags = CanvasPixels_Straight;
    else if (alpha == PrDrawAlpha_Opaque)
        flags = CanvasPixels_Opaque;
    const uint8_t* texels = pixels.data;
    size_t stride = pixels.stride;
    if (format == PrPixelFormat_Bgra8) {
        flags |= CanvasPixels_Bgra;
    } else if (format != PrPixelFormat_Rgba8) {
        stride = size_t(pixels.extent.x) * 4;
        uint8_t* copy = scratchTexelsFor(backend, stride * pixels.extent.y);
        prDrawImageWritePixels(image, PrPixels{copy, stride, pixels.extent}, PrPixelFormat_Rgba8, PrIVec2{0, 0});
        texels = copy;
        flags = 0;
    }
    *out = Source{
        .words = {
            CanvasSource_Image, uint32_t(id), uint32_t(id >> 32), uint32_t(uintptr_t(texels)), uint32_t(stride),
            pixels.extent.x, pixels.extent.y, flags,
        },
        .extent = pixels.extent,
    };
}

// Draws an area of a list into a surface of the play, at `scale` target pixels per unit of `tile` along each axis. A
// play draws each area and size once. Returns the list's index, or ~0 for an area with no pixels.
uint32_t drawList(Walk& w, const PrDrawList* list, PrRect area, PrRect tile, PrVec2 scale, PrUVec2* size) {
    *size = prDrawLayerExtent(PrVec2{prRectWidth(tile), prRectHeight(tile)}, scale, kListMaxSize);
    if (size->x == 0 || prRectIsEmpty(area))
        return ~0u;
    uint64_t id = prDrawListId(list);
    for (const DrawnList& d : w.drawn->span()) {
        if (d.id == id && d.size.x == size->x && d.size.y == size->y && d.area.left == area.left &&
                d.area.top == area.top && d.area.right == area.right && d.area.bottom == area.bottom)
            return d.index;
    }
    prDrawCanvasPushList(size->x, size->y);
    Walk nested{.backend = w.backend, .drawn = w.drawn};
    forgetState(nested);
    PrAffine2 place = prAffine2Concat(prAffine2Translate(PrVec2{-area.left, -area.top}),
        prAffine2Scale(PrVec2{float(size->x) / prRectWidth(area), float(size->y) / prRectHeight(area)}));
    nested.transforms.push(prDrawTransformFromAffine2(place));
    nested.clips.push(PrIRect{0, 0, int32_t(size->x), int32_t(size->y)});
    runCommands(nested, prDrawListCommands(list), prDrawListCommandCount(list));
    nested.transforms.reset();
    nested.clips.reset();
    nested.groups.reset();
    uint32_t index = prDrawCanvasPopList();
    w.drawn->push(DrawnList{id, area, *size, index});
    return index;
}

// Resolves a source drawn with `scale` target pixels per unit of `tile`. Returns false for a source that draws nothing.
bool resolveSource(Walk& w, const PrDrawSource& source, PrRect tile, PrVec2 scale, Source* out) {
    *out = Source{};
    if (!prDrawSourceIsValid(source))
        return false;
    switch (source.kind) {
        case PrDrawSourceKind_Image:
            imageSource(w.backend, source.image, out);
            out->texels = source.rect;
            return true;
        case PrDrawSourceKind_Texture: {
            PrGpuCanvas canvas = 0;
            if (!prGpuTextureNative(w.backend->device, PrGpuTexture{uint32_t(source.texture)}, PrGpuNativeKind_Canvas2d, &canvas))
                return false;
            *out = Source{.words = {CanvasSource_Texture, canvas}, .texels = source.rect, .extent = source.extent};
            return true;
        }
        default: {
            PrUVec2 size{};
            uint32_t index = drawList(w, source.list, source.rect, tile, scale, &size);
            if (index == ~0u)
                return false;
            *out = Source{
                .words = {CanvasSource_List, index},
                .texels = PrRect{0, 0, float(size.x), float(size.y)},
                .extent = size,
            };
            return true;
        }
    }
}

CanvasSampling canvasSampling(PrDrawSampling sampling) {
    switch (sampling & PrDrawSamplingFlag_Kind) {
        case PrDrawSampling_Nearest: return CanvasSampling_Nearest;
        case PrDrawSampling_Linear: return CanvasSampling_Low;
        default: return CanvasSampling_High;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// How a draw places its paths. Without a brush, the path's own transform places it in target pixels. With a brush, the
// path is placed in the brush's space, which `toTarget` maps to target pixels.
struct Paint {
    bool brushed;
    PrAffine2 toTarget;
    PrAffine2 fromTarget;
};

// Sets the transform, and maps the current path for the paint. `m` maps the path's space to target pixels.
void placePath(const Paint& paint, PrAffine2 m) {
    if (!paint.brushed) {
        sendTransform(m);
        return;
    }
    sendTransform(paint.toTarget);
    sendMapPath(prAffine2Concat(m, paint.fromTarget));
}

// A gradient's ramp, from which its stops are taken.
struct Ramp {
    RawArray<PrDrawRampPiece> pieces;
    RawArray<PrColor> colors;
    uint32_t pieceCount;
};

// Returns the gradient's color at `t`, before the gradient extends.
PrColor rampAt(const Ramp& s, float t) {
    uint32_t piece = prDrawRampPieceAt(s.pieces.data, t, 0);
    return prDrawRampColor(&s.pieces.data[piece], s.colors.data, t);
}

// Adds one stop at `t`, as an offset in [0, 1] of the range from `from` to `to`. The stop's color is the premultiplied
// `c`, unpremultiplied.
void addStop(RawArray<float>& out, float t, float from, float to, PrColor c) {
    float a = prF32Clamp(c.a, 0, 1);
    float scale = a > 0 ? 1 / a : 0;
    out.push(prF32Clamp((t - from) / (to - from), 0, 1));
    out.push(prF32Clamp(c.r * scale, 0, 1));
    out.push(prF32Clamp(c.g * scale, 0, 1));
    out.push(prF32Clamp(c.b * scale, 0, 1));
    out.push(a);
}

// Adds the stops of one period of the gradient, [0, 1], placed at [at, at + 1], reflected if `reflect`, and kept within
// [from, to]. A stop lies at each point where the gradient changes how it mixes, and close enough to the next stop that
// the context mixes between them as the gradient does.
void addPeriod(RawArray<float>& out, const Ramp& s, float at, bool reflect, float from, float to) {
    // The pieces in order along the period, backward if the period is reflected.
    for (uint32_t k = 0; k < s.pieceCount; ++k) {
        uint32_t index = reflect ? s.pieceCount - 1 - k : k;
        const PrDrawRampPiece& p = s.pieces.data[index];
        float start = prF32Max(p.start, 0);
        float end = prF32Min(p.end, 1);
        if (!(start <= end))
            continue;
        const PrColor* colors = s.colors.data + p.first;
        uint32_t steps = 1;
        if (p.count > 1) {
            bool varies = colors[0].a != colors[p.count - 1].a;
            steps = varies && p.count - 1 < kAlphaSteps ? kAlphaSteps : p.count - 1;
        }
        for (uint32_t i = 0; i <= steps; ++i) {
            uint32_t j = reflect ? steps - i : i;
            float u = start + (end - start) * float(j) / float(steps);
            float t = at + (reflect ? 1 - u : u);
            if (t >= from && t <= to)
                addStop(out, t, from, to, prDrawRampColor(&p, s.colors.data, prF32Clamp(u, p.start, p.end)));
        }
    }
}

// Fills `out` with the stops of a gradient over its parameter's range [from, to], and returns the number of stops.
uint32_t writeStops(RawArray<float>& out, const Ramp& s, PrDrawExtend extend, float from, float to) {
    out.clear();
    if (extend == PrDrawExtend_Pad) {
        addPeriod(out, s, 0, false, 0, 1);
        return out.count / 5;
    }
    // The range's ends, and then each period within it.
    auto edge = [&](float t) {
        float u = t;
        PrColor c{};
        if (prDrawGradientExtend(extend, &u))
            c = rampAt(s, u);
        addStop(out, t, from, to, c);
    };
    edge(from);
    for (float at = prF32Floor(from); at < to; ++at) {
        if (extend == PrDrawExtend_None && (at < 0 || at >= 1)) {
            // Outside its range, the gradient paints nothing. Transparent stops cut sharply at the period's ends.
            addStop(out, prF32Max(at, from), from, to, PrColor{});
            addStop(out, prF32Min(at + 1, to), from, to, PrColor{});
            continue;
        }
        bool reflect = extend == PrDrawExtend_Reflect && (int64_t(at) & 1) != 0;
        addPeriod(out, s, at, reflect, from, to);
    }
    edge(to);
    return out.count / 5;
}

// Fills `s` with the gradient's ramp for a command of `color`.
void buildRamp(const PrDrawBrush* brush, PrColor color, Ramp* s) {
    s->pieces.resize(prDrawGradientRampMaxPieces(brush->stopCount));
    s->colors.resize(prDrawGradientRampMaxColors(brush->stopCount, kRampResolution));
    s->pieceCount =
        prDrawGradientRamp(brush, color, PrColorSpace_Srgb, kRampResolution, s->pieces.data, s->colors.data);
}

// Returns the range of the gradient's parameter over `bounds`, a rect in user space, or false if the range has too many
// periods to draw as one gradient. A radial gradient's parameter is sampled at the bounds' corners and edge midpoints,
// and at zero.
bool parameterRange(const PrDrawBrush* brush, PrAffine2 userToBrush, PrRect bounds, float* from, float* to) {
    float lo = 0;
    float hi = 1;
    const float xs[] = {bounds.left, (bounds.left + bounds.right) / 2, bounds.right};
    const float ys[] = {bounds.top, (bounds.top + bounds.bottom) / 2, bounds.bottom};
    for (float y : ys) {
        for (float x : xs) {
            float t = 0;
            if (prDrawGradientParameter(brush, prAffine2MapPoint(userToBrush, PrVec2{x, y}), &t) && prF32IsFinite(t)) {
                lo = prF32Min(lo, t);
                hi = prF32Max(hi, t);
            }
        }
    }
    if (brush->kind == PrDrawBrushKind_Radial) {
        // A radial gradient's circles keep growing past the bounds' farthest point, which the sampled points may miss.
        hi += 1;
    }
    *from = prF32Floor(lo);
    *to = prF32Ceil(hi);
    return *to - *from <= kMaxPeriods;
}

// Sets a pattern of the brush's texels over a box of target pixels, evaluated per pixel.
void brushPixels(Walk& w, const PrDrawBrush* brush, PrColor color, PrAffine2 fromTarget, PrIRect box, Paint* out) {
    uint32_t width = prIRectWidth(box);
    uint32_t height = prIRectHeight(box);
    size_t stride = size_t(width) * 4;
    uint8_t* texels = scratchTexelsFor(w.backend, stride * height);
    Ramp s{};
    buildRamp(brush, color, &s);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            PrVec2 p{float(box.left + int32_t(x)) + 0.5f, float(box.top + int32_t(y)) + 0.5f};
            float t = 0;
            PrColor c{};
            if (prDrawGradientParameter(brush, prAffine2MapPoint(fromTarget, p), &t) &&
                    prDrawGradientExtend(brush->extend, &t))
                c = rampAt(s, t);
            uint8_t* texel = texels + y * stride + x * 4;
            texel[0] = uint8_t(prF32Clamp(c.r, 0, 1) * 255 + 0.5f);
            texel[1] = uint8_t(prF32Clamp(c.g, 0, 1) * 255 + 0.5f);
            texel[2] = uint8_t(prF32Clamp(c.b, 0, 1) * 255 + 0.5f);
            texel[3] = uint8_t(prF32Clamp(c.a, 0, 1) * 255 + 0.5f);
        }
    }
    s.pieces.reset();
    s.colors.reset();
    const uint32_t source[8] = {CanvasSource_Pixels, uint32_t(uintptr_t(texels)), uint32_t(stride), width, height, 0};
    const uint32_t outset[4] = {};
    prDrawCanvasPattern(source, 0, 0, float(width), float(height), PrDrawExtend_None, PrDrawExtend_None, outset, 1, 0, 0, 1,
        float(box.left), float(box.top), CanvasSampling_Nearest);
    *out = Paint{.brushed = true, .toTarget = prAffine2Identity(), .fromTarget = prAffine2Identity()};
}

// Sets a gradient brush's paint. Returns false if it paints nothing.
bool setGradient(Walk& w, const PrDrawBrush* brush, PrColor color, PrRect bounds, PrAffine2 fromTarget,
        PrAffine2 toTarget, Paint* out) {
    const PrAffine2 userToBrush = prAffine2Concat(current(w).affine, fromTarget);
    float from = 0;
    float to = 1;
    PrDrawExtend extend = brush->extend;
    if (brush->kind == PrDrawBrushKind_Diamond || (extend != PrDrawExtend_Pad && brush->kind != PrDrawBrushKind_Conic &&
            !parameterRange(brush, userToBrush, bounds, &from, &to))) {
        PrIRect box = prIRectIntersect(deviceBox(current(w).affine, bounds), clipBox(w));
        if (prIRectIsEmpty(box))
            return false;
        brushPixels(w, brush, color, fromTarget, box, out);
        return true;
    }
    if (brush->kind == PrDrawBrushKind_Conic)
        extend = PrDrawExtend_Pad;
    Ramp s{};
    buildRamp(brush, color, &s);
    RawArray<float>& stops = w.backend->operands;
    switch (brush->kind) {
        case PrDrawBrushKind_Linear: {
            uint32_t count = writeStops(stops, s, extend, from, to);
            PrVec2 a = brush->linear.start;
            PrVec2 d{brush->linear.end.x - a.x, brush->linear.end.y - a.y};
            prDrawCanvasLinear(a.x + d.x * from, a.y + d.y * from, a.x + d.x * to, a.y + d.y * to, stops.data, count);
            break;
        }
        case PrDrawBrushKind_Radial: {
            const PrDrawRadialGradient& r = brush->radial;
            // The context draws no circle of negative radius, so the range starts where the radius reaches zero.
            float dr = r.endRadius - r.startRadius;
            if (r.startRadius + dr * from < 0 && dr != 0)
                from = -r.startRadius / dr;
            uint32_t count = writeStops(stops, s, extend, from, to);
            PrVec2 d{r.end.x - r.start.x, r.end.y - r.start.y};
            prDrawCanvasRadial(r.start.x + d.x * from, r.start.y + d.y * from, prF32Max(r.startRadius + dr * from, 0),
                r.start.x + d.x * to, r.start.y + d.y * to, prF32Max(r.startRadius + dr * to, 0), stops.data, count);
            break;
        }
        default: {
            uint32_t count = writeStops(stops, s, extend, from, to);
            prDrawCanvasConic(brush->conic.angle, brush->conic.center.x, brush->conic.center.y, stops.data, count);
            break;
        }
    }
    s.pieces.reset();
    s.colors.reset();
    *out = Paint{.brushed = true, .toTarget = toTarget, .fromTarget = fromTarget};
    return true;
}

// The most texels that a padded pattern extends its crop by on a side. A fill that reaches farther past the tile draws
// nothing there.
constexpr uint32_t kMaxPad = 2048;

// Sets an image brush's paint, a pattern over its tile. `bounds` are the bounds of the fill geometry in user space.
// Returns false if it paints nothing.
bool setImageBrush(Walk& w, const PrDrawBrush* brush, PrColor color, PrRect bounds, PrAffine2 fromTarget,
        PrAffine2 toTarget, Paint* out) {
    const PrDrawImageFill& fill = brush->image;
    PrRect tile = fill.tiles.tile;
    if (prRectIsEmpty(tile))
        return false;
    PrVec2 scale{prF32Hypot(toTarget.a, toTarget.b), prF32Hypot(toTarget.c, toTarget.d)};
    Source source{};
    if (!resolveSource(w, fill.source, tile, scale, &source))
        return false;
    // The crop covers every texel that the source's rect touches, mapped onto the tile.
    PrRect texels = source.texels;
    float cropLeft = prF32Max(prF32Floor(texels.left), 0);
    float cropTop = prF32Max(prF32Floor(texels.top), 0);
    float cropRight = prF32Min(prF32Ceil(texels.right), float(source.extent.x));
    float cropBottom = prF32Min(prF32Ceil(texels.bottom), float(source.extent.y));
    if (!(cropLeft < cropRight && cropTop < cropBottom))
        return false;
    float sx = prRectWidth(tile) / prRectWidth(texels);
    float sy = prRectHeight(tile) / prRectHeight(texels);
    // A padded axis extends its edge texels past the crop, over every texel that the fill reaches, and one more, which
    // linear sampling reads at the crop's edge. The context's patterns repeat or end, so the page builds the padding.
    uint32_t outset[4] = {};
    if (fill.tiles.x == PrDrawExtend_Pad || fill.tiles.y == PrDrawExtend_Pad) {
        PrIRect box = prIRectIntersect(clipBox(w), deviceBox(current(w).affine, bounds));
        PrRect reached = prAffine2MapRect(fromTarget,
            PrRect{float(box.left), float(box.top), float(box.right), float(box.bottom)});
        float left = texels.left + (reached.left - tile.left) / sx;
        float top = texels.top + (reached.top - tile.top) / sy;
        float right = texels.left + (reached.right - tile.left) / sx;
        float bottom = texels.top + (reached.bottom - tile.top) / sy;
        auto pad = [](float beyond) { return uint32_t(prF32Clamp(prF32Ceil(beyond), 0, float(kMaxPad - 1))) + 1; };
        if (fill.tiles.x == PrDrawExtend_Pad) {
            outset[0] = pad(cropLeft - left);
            outset[2] = pad(right - cropRight);
        }
        if (fill.tiles.y == PrDrawExtend_Pad) {
            outset[1] = pad(cropTop - top);
            outset[3] = pad(bottom - cropBottom);
        }
    }
    setAlpha(w, prF32Clamp(color.a, 0, 1));
    prDrawCanvasPattern(source.words, cropLeft, cropTop, cropRight - cropLeft, cropBottom - cropTop, fill.tiles.x,
        fill.tiles.y, outset, sx, 0, 0, sy, tile.left + (cropLeft - texels.left) * sx,
        tile.top + (cropTop - texels.top) * sy, canvasSampling(fill.sampling));
    *out = Paint{.brushed = true, .toTarget = toTarget, .fromTarget = fromTarget};
    return true;
}

// Sets the paint of a draw: its color, or its brush times its color. `bounds` are the bounds of the fill geometry in
// user space, for a brush in ObjectBounds. Returns false if the draw paints nothing.
bool setPaint(Walk& w, const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds, PrDrawComposite composite,
        PrDrawBlendMode blend, Paint* out) {
    setOperation(w, composite, blend);
    // Clear removes what lies under the coverage, which an opaque source does with destination-out.
    if (composite == PrDrawComposite_Clear) {
        setAlpha(w, 1);
        sendColor(PrColor{0, 0, 0, 1});
        *out = Paint{};
        return true;
    }
    if (brush == nullptr) {
        if (!(color.a > 0) && !prDrawCompositeIsUnbounded(composite))
            return false;
        setAlpha(w, 1);
        sendColor(color);
        *out = Paint{};
        return true;
    }
    PrAffine2 fromTarget{};
    PrAffine2 toTarget{};
    if (!prDrawBrushFromTarget(brush, current(w).affine, bounds, &fromTarget) ||
            !prAffine2Invert(fromTarget, &toTarget))
        return false;
    if (brush->kind == PrDrawBrushKind_Image)
        return setImageBrush(w, brush, color, bounds, fromTarget, toTarget, out);
    setAlpha(w, 1);
    return setGradient(w, brush, color, bounds, fromTarget, toTarget, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes and strokes

// Returns true if the context strokes `stroke` as lib/draw defines it: centered, scaling with the transform, painted
// plainly, with one cap for every end, and with a cap and a join that the context has.
bool strokesNatively(const PrDrawStroke& stroke) {
    const PrPathStroke& s = stroke.shape;
    if (stroke.nonScaling || stroke.mapping != PrDrawStrokeMapping_Plain || s.profile != nullptr ||
            stroke.align != PrDrawStrokeAlign_Center)
        return false;
    if (s.join != PrPathJoin_Miter && s.join != PrPathJoin_Round && s.join != PrPathJoin_Bevel)
        return false;
    if (s.startCap != s.endCap || s.startCap > PrPathCap_Square)
        return false;
    if (s.dashCount != 0) {
        if (s.dashCap != s.startCap)
            return false;
        float sum = 0;
        for (uint32_t i = 0; i < s.dashCount; ++i) {
            if (!(s.dashes[i] >= 0))
                return false;
            sum += s.dashes[i];
        }
        if (!(sum > 0))
            return false;
    }
    return true;
}

// Strokes the current path with the context. The page numbers the caps butt, round and square, and the joins miter,
// round and bevel, in that order.
void sendStroke(const PrPathStroke& s) {
    uint32_t join = 2;
    if (s.join == PrPathJoin_Miter)
        join = 0;
    else if (s.join == PrPathJoin_Round)
        join = 1;
    prDrawCanvasStroke(s.width, s.startCap, join, s.miterLimit, s.dashOffset, s.dashes, s.dashCount);
}

// Fills polylines in target pixels with the paint.
void fillTargetPolyline(const Paint& paint, const PrPathPolyline* polyline, PrPathFillRule rule) {
    if (polyline->pointCount == 0)
        return;
    sendPolyline(polyline);
    placePath(paint, prAffine2Identity());
    prDrawCanvasFill(canvasRule(rule));
}

// Strokes `path`, in user space, as `stroke` says, lowered into fills in target pixels.
void strokeLowered(Walk& w, const Paint& paint, const PrPath* path, const PrDrawStroke& stroke) {
    CanvasBackend* backend = w.backend;
    prPathPolylineClear(&backend->fill);
    prPathPolylineClear(&backend->aligned);
    prPathPolylineClear(&backend->region);
    prDrawLowerStroke(path, &stroke, current(w).affine, kTolerance, &backend->fill, &backend->aligned,
        &backend->region);
    fillTargetPolyline(paint, &backend->fill, PrPathFillRule_NonZero);
    if (backend->aligned.pointCount == 0)
        return;
    // An aligned stroke is clipped to the inside or the outside of its closed contours.
    prDrawCanvasSave();
    if (stroke.align == PrDrawStrokeAlign_Outside) {
        PrRect around = prRectFromIRect(prIRectOutset(clipBox(w), 1));
        prPathPolylineMoveTo(&backend->region, PrVec2{around.left, around.top});
        prPathPolylineLineTo(&backend->region, PrVec2{around.right, around.top});
        prPathPolylineLineTo(&backend->region, PrVec2{around.right, around.bottom});
        prPathPolylineLineTo(&backend->region, PrVec2{around.left, around.bottom});
        prPathPolylineClose(&backend->region);
    }
    sendPolyline(&backend->region);
    sendTransform(prAffine2Identity());
    prDrawCanvasClip(stroke.align == PrDrawStrokeAlign_Outside ? 1 : 0);
    fillTargetPolyline(paint, &backend->aligned, PrPathFillRule_NonZero);
    restore(w);
}

// Fills or strokes `path`, in user space, with the paint.
void drawPath(Walk& w, const Paint& paint, const PrPath* path, PrPathFillRule rule, const PrDrawStroke* PR_NULLABLE stroke) {
    if (stroke == nullptr) {
        sendPath(path);
        placePath(paint, current(w).affine);
        prDrawCanvasFill(canvasRule(rule));
        return;
    }
    if (!(stroke->shape.width > 0))
        return;
    // The context strokes in the space of its transform, so it would distort a stroke under a brush's transform.
    if (!paint.brushed && strokesNatively(*stroke)) {
        sendPath(path);
        sendTransform(current(w).affine);
        sendStroke(stroke->shape);
        return;
    }
    strokeLowered(w, paint, path, *stroke);
}

void drawShape(Walk& w, const PrDrawCommand& c) {
    Paint paint{};
    if (!setPaint(w, c.brush, prColorFromColor16(c.color), prDrawFillBounds(&c), c.composite, c.blendMode, &paint))
        return;
    prPathClear(w.backend->path);
    prDrawShapePath(&c, w.backend->path);
    drawPath(w, paint, w.backend->path, c.rule, c.stroke);
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

struct BorderDraw {
    Walk* w;
    const PrDrawCommand* command;
};

void drawBorderPiece(void* PR_NULLABLE context, const PrDrawBorderPiece* piece, const PrPathPolyline* shape,
        const PrPathPolyline* PR_NULLABLE cut) {
    BorderDraw& d = *static_cast<BorderDraw*>(context);
    Walk& w = *d.w;
    const PrDrawCommand& c = *d.command;
    const PrDrawBorderSide& side = c.border->sides[piece->side];
    if (cut != nullptr) {
        prDrawCanvasSave();
        sendPolyline(cut);
        sendTransform(current(w).affine);
        prDrawCanvasClip(0);
    }
    Paint paint{};
    if (setPaint(w, side.brush, piece->color, c.border->shape.rect, c.composite, c.blendMode, &paint)) {
        sendPolyline(shape);
        placePath(paint, current(w).affine);
        prDrawCanvasFill(canvasRule(piece->rule));
    }
    if (cut != nullptr)
        restore(w);
}

void drawBorder(Walk& w, const PrDrawCommand& c) {
    float scale = prAffine2MaxScale(current(w).affine);
    if (!(scale > 0))
        return;
    BorderDraw d{&w, &c};
    prDrawBorderLower(c.border, kTolerance / scale, drawBorderPiece, &d);
}

void drawShadow(Walk& w, const PrDrawCommand& c) {
    CanvasBackend* backend = w.backend;
    const PrDrawTransform& x = current(w);
    PrDrawShadowKind kind = c.kind == PrDrawCommandKind_InnerShadow ? PrDrawShadowKind_Inner : PrDrawShadowKind_Drop;
    PrDrawShadowGeometry g = prDrawShadowGeometry(c.shadow, kind);
    float scale = c.shadow->nonScaling ? 1 : prAffine2MaxScale(x.affine);
    float sigma = g.sigma * scale;
    PrAffine2 shifted = x.affine;
    if (c.shadow->nonScaling)
        shifted = prAffine2Concat(x.affine, prAffine2Translate(c.shadow->offset));
    PrColor shade = prColorFromColor16(c.color);
    if (c.brush != nullptr)
        shade = PrColor{0, 0, 0, shade.a};
    if (!(shade.a > 0))
        return;
    setOperation(w, c.composite, c.blendMode);
    setAlpha(w, 1);
    // The caster in target pixels, which clips the shadow.
    PrPath* caster = backend->extra;
    prPathClear(caster);
    prPathAddCornerRect(caster, c.shadow->shape);
    prPathTransform(caster, x.affine);
    PrPath* shape = backend->path;
    prPathClear(shape);
    prPathAddCornerRect(shape, g.shape);
    prPathTransform(shape, shifted);
    float reach = sigma * 3 + 2;
    PrRect shapeBounds = prPathBounds(shape);
    prDrawCanvasSave();
    sendTransform(prAffine2Identity());
    if (kind == PrDrawShadowKind_Drop) {
        // A drop shadow lies outside the caster: everywhere around it, less the caster.
        prPathAddRect(caster, prRectFromIRect(prIRectOutset(clipBox(w), 1)));
        sendPath(caster);
        prDrawCanvasClip(1);
    } else {
        // An inner shadow lies inside the caster: the shadow of what lies outside the opening.
        sendPath(caster);
        prDrawCanvasClip(0);
        PrRect casterBounds = prPathBounds(caster);
        PrRect around{
            casterBounds.left - reach, casterBounds.top - reach, casterBounds.right + reach,
            casterBounds.bottom + reach,
        };
        around = prRectUnion(around, shapeBounds);
        prPathAddRect(shape, around);
        shapeBounds = around;
    }
    sendPath(shape);
    prDrawCanvasShadowFill(-(prF32Ceil(shapeBounds.right + reach) + 1), sigma, prF32Clamp(shade.r, 0, 1),
        prF32Clamp(shade.g, 0, 1), prF32Clamp(shade.b, 0, 1), prF32Clamp(shade.a, 0, 1), kind == PrDrawShadowKind_Drop ? 0 : 1);
    restore(w);
}

void drawDecoration(Walk& w, const PrDrawCommand& c) {
    CanvasBackend* backend = w.backend;
    float scale = prAffine2MaxScale(current(w).affine);
    if (!(scale > 0))
        return;
    Paint paint{};
    if (!setPaint(w, c.brush, prColorFromColor16(c.color), prDrawFillBounds(&c), c.composite, c.blendMode, &paint))
        return;
    prPathPolylineClear(&backend->fill);
    prDrawDecorationOutline(c.decoration, kTolerance / scale, &backend->fill);
    if (backend->fill.pointCount == 0)
        return;
    sendPolyline(&backend->fill);
    placePath(paint, current(w).affine);
    prDrawCanvasFill(0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

void drawNineSlice(Walk& w, const PrDrawCommand& c) {
    const PrDrawSource& source = c.nineSlice.source;
    PrVec2 size{prRectWidth(source.rect), prRectHeight(source.rect)};
    if (!(size.x > 0 && size.y > 0))
        return;
    PrAffine2 m = current(w).affine;
    PrVec2 scale{
        prF32Hypot(m.a, m.b) * prRectWidth(c.nineSlice.destination) / size.x,
        prF32Hypot(m.c, m.d) * prRectHeight(c.nineSlice.destination) / size.y,
    };
    Source resolved{};
    if (!resolveSource(w, source, source.rect, scale, &resolved))
        return;
    // The source's rect maps onto the resolved texels. A list's texels are its whole surface.
    float toX = prRectWidth(resolved.texels) / size.x;
    float toY = prRectHeight(resolved.texels) / size.y;
    constexpr uint32_t kMaxRects = 64;
    PrDrawImageRect rects[kMaxRects];
    uint32_t count = prDrawNineSliceRects(c.nineSlice.slices, size, c.nineSlice.destination, rects, kMaxRects);
    if (count > kMaxRects)
        count = kMaxRects;
    setOperation(w, c.composite, c.blendMode);
    setAlpha(w, prF32Clamp(prColorFromColor16(c.color).a, 0, 1));
    sendTransform(m);
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawImageRect& r = rects[i];
        if (prRectIsEmpty(r.source) || prRectIsEmpty(r.destination))
            continue;
        prDrawCanvasDrawImage(resolved.words, resolved.texels.left + r.source.left * toX,
            resolved.texels.top + r.source.top * toY, prRectWidth(r.source) * toX, prRectHeight(r.source) * toY,
            r.destination.left, r.destination.top, prRectWidth(r.destination), prRectHeight(r.destination),
            canvasSampling(c.sampling));
    }
}

void writePixels(Walk& w, const PrDrawCommand& c) {
    Source source{};
    imageSource(w.backend, c.writePixels.image, &source);
    // The words of the image's source after its kind and id: pointer, stride, width, height and flags.
    prDrawCanvasPutPixels(reinterpret_cast<const uint8_t*>(uintptr_t(source.words[3])), source.words[4],
        source.words[5], source.words[6], c.writePixels.origin.x, c.writePixels.origin.y, source.words[7]);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Returns the glyph's outline in ems, made the first time that any play draws the glyph, or null for a glyph with no
// outline.
CanvasGlyph* findGlyph(CanvasBackend* backend, const PrFont* font, uint16_t glyph) {
    uint64_t key = (prFontId(font) << 16) | glyph;
    uint32_t hash = KeyIndex::hashOf(key);
    uint32_t found = backend->glyphIndex.find(hash, [&](uint32_t at) { return backend->glyphs.data[at].key == key; });
    if (found != kHashMissing) {
        CanvasGlyph* g = &backend->glyphs.data[found];
        g->used = backend->serial;
        return g->path != nullptr ? g : nullptr;
    }
    uint32_t slot = 0;
    if (backend->freeGlyphs.count != 0) {
        slot = backend->freeGlyphs.last();
        backend->freeGlyphs.pop();
    } else {
        slot = backend->glyphs.count;
        backend->glyphs.push(CanvasGlyph{});
    }
    CanvasGlyph& g = backend->glyphs.data[slot];
    g = CanvasGlyph{.key = key, .id = ++backend->nextGlyphId, .used = backend->serial};
    PrPath* path = prPathCreate();
    if (prFontGlyphOutline(font, glyph, 0, prAffine2Identity(), path))
        g.path = path;
    else
        prPathDestroy(path);
    backend->glyphIndex.insert(hash, slot);
    return g.path != nullptr ? &g : nullptr;
}

// Drops the glyphs that no play has drawn for a while.
void dropGlyphs(CanvasBackend* backend) {
    for (uint32_t i = 0; i < backend->glyphs.count; ++i) {
        CanvasGlyph& g = backend->glyphs.data[i];
        if (g.id == 0 || backend->serial - g.used < kGlyphAge)
            continue;
        backend->glyphIndex.erase(KeyIndex::hashOf(g.key), i);
        prPathDestroy(g.path);
        g = CanvasGlyph{};
        backend->freeGlyphs.push(i);
    }
}

void drawGlyphs(Walk& w, const PrDrawCommand& c) {
    CanvasBackend* backend = w.backend;
    const PrFontGlyphRun* run = c.glyphs;
    PrColor foreground = prColorFromColor16(c.color);
    foreground.a = 1;
    Paint paint{};
    bool painted = false;
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        uint16_t glyph = run->glyphs[i].id;
        PrAffine2 x = current(w).affine;
        if (c.stroke == nullptr) {
            float pixelsPerEm = prDrawGlyphPixelsPerEm(run, i, x);
            PrDrawList* list =
                prDrawGlyphCacheList(backend->glyphCache, run->font, glyph, pixelsPerEm, &run->palette, foreground);
            if (list != nullptr) {
                PrDrawGroup group{};
                PrDrawCommand commands[kPrDrawGlyphMaxCommands];
                runCommands(w, commands, prDrawGlyphListCommands(&c, i, list, &group, commands));
                painted = false;
                forgetState(w);
                continue;
            }
        }
        if (!painted) {
            if (!setPaint(w, c.brush, prColorFromColor16(c.color), prDrawFillBounds(&c), c.composite, c.blendMode, &paint))
                return;
            painted = true;
        }
        if (c.stroke != nullptr) {
            prPathClear(backend->path);
            if (prDrawGlyphPath(run, i, nullptr, backend->path))
                drawPath(w, paint, backend->path, PrPathFillRule_NonZero, c.stroke);
            continue;
        }
        CanvasGlyph* g = findGlyph(backend, run->font, glyph);
        if (g == nullptr)
            continue;
        prDrawCanvasGlyph(g->id, prPathVerbs(g->path), prPathVerbCount(g->path), prPathPoints(g->path));
        placePath(paint, prAffine2Concat(prDrawGlyphTransform(run, i), x));
        prDrawCanvasFill(0);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Clips and groups

void pushClip(Walk& w, const PrPath* shape, PrAffine2 m, PrPathFillRule rule) {
    prDrawCanvasSave();
    sendPath(shape);
    sendTransform(m);
    prDrawCanvasClip(canvasRule(rule));
    w.clips.push(prIRectIntersect(clipBox(w), deviceBox(m, prPathBounds(shape))));
}

// Fills `out` with the filters that the context applies for `filter`, in target pixels under `m`, and returns their
// number. The context applies a chain of blurs and drop shadows of the contents. Any other graph has no filters, and
// the group draws unfiltered.
uint32_t writeFilters(RawArray<float>& out, const PrDrawFilter* PR_NULLABLE filter, PrAffine2 m) {
    out.clear();
    if (filter == nullptr)
        return 0;
    float scale = prAffine2MaxScale(m);
    const PrDrawFilterNode* nodes = prDrawFilterNodes(filter);
    uint32_t count = prDrawFilterNodeCount(filter);
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawFilterNode& n = nodes[i];
        int32_t previous = i == 0 ? PrDrawFilterInput_Source : int32_t(i - 1);
        if (n.inputs[0] != previous)
            return 0;
        if (n.kind == PrDrawFilterKind_Blur) {
            out.push(float(CanvasFilter_Blur));
            out.push((n.blur.sigma.x + n.blur.sigma.y) / 2 * scale);
        } else if (n.kind == PrDrawFilterKind_DropShadow) {
            PrColor c = prColorFromColor16(n.shadow.color);
            const float values[] = {
                float(CanvasFilter_DropShadow),
                m.a * n.shadow.offset.x + m.c * n.shadow.offset.y,
                m.b * n.shadow.offset.x + m.d * n.shadow.offset.y,
                (n.shadow.sigma.x + n.shadow.sigma.y) / 2 * scale,
                prF32Clamp(c.r, 0, 1), prF32Clamp(c.g, 0, 1), prF32Clamp(c.b, 0, 1), prF32Clamp(c.a, 0, 1),
            };
            out.append(values, 8);
        } else {
            return 0;
        }
    }
    return count;
}

// Pushes a group. Returns the index of the command to continue after. For a group that covers nothing, that is the
// group's pop.
uint32_t pushGroup(Walk& w, const PrDrawCommand* commands, uint32_t count, uint32_t index) {
    const PrDrawCommand& c = commands[index];
    const PrDrawGroup* g = c.group;
    Group group{
        .layered = prDrawGroupHasEffect(g),
        .group = g,
        .composite = c.composite,
        .blendMode = c.blendMode,
        .transform = current(w),
        .clipDepth = w.clips.count,
    };
    if (!group.layered) {
        w.groups.push(group);
        return index;
    }
    PrIRect box = clipBox(w);
    if (!prRectIsEmpty(g->bounds) && !prDrawCompositeIsUnbounded(c.composite))
        box = prIRectIntersect(box, deviceBox(current(w).affine, g->bounds));
    // The context cannot map a projected plane through its projection, so a projected group draws nothing.
    if (prIRectIsEmpty(box) || g->projection != nullptr)
        return prDrawCommandGroupEnd(commands, count, index);
    group.box = box;
    prDrawCanvasPushLayer(box.left, box.top, prIRectWidth(box), prIRectHeight(box));
    if (g->backdrop != nullptr) {
        uint32_t filters = writeFilters(w.backend->operands, g->backdrop, current(w).affine);
        prDrawCanvasBackdrop(w.backend->operands.data, filters);
    }
    forgetState(w);
    w.groups.push(group);
    w.clips.push(box);
    return index;
}

void popGroup(Walk& w) {
    Group group = w.groups.last();
    w.groups.pop();
    if (!group.layered)
        return;
    w.clips.count = group.clipDepth;
    const PrDrawGroup* g = group.group;
    if (g->mask != nullptr) {
        // The mask is the mask's brush in the group's space, over the layer.
        prDrawCanvasPushMask();
        forgetState(w);
        w.transforms.push(group.transform);
        w.clips.push(group.box);
        Paint paint{};
        if (setPaint(w, &g->mask->brush, PrColor{1, 1, 1, 1}, g->bounds, PrDrawComposite_SourceOver,
                PrDrawBlendMode_Normal, &paint)) {
            PrRect box = prRectFromIRect(group.box);
            prDrawCanvasRect(box.left, box.top, box.right, box.bottom);
            placePath(paint, prAffine2Identity());
            prDrawCanvasFill(0);
        }
        w.clips.pop();
        w.transforms.pop();
        prDrawCanvasPopMask(g->mask->mode == PrDrawMaskMode_Luminance, g->mask->invert);
    }
    uint32_t filters = writeFilters(w.backend->operands, g->filter, group.transform.affine);
    prDrawCanvasPopLayer(prF32Clamp(g->opacity, 0, 1), canvasOperation(group.composite, group.blendMode),
        w.backend->operands.data, filters);
    forgetState(w);
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

void runCommands(Walk& w, const PrDrawCommand* commands, uint32_t count) {
    CanvasBackend* backend = w.backend;
    PrDrawSnapped snapped;
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = *prDrawTransformSnap(&current(w), &commands[i], &snapped);
        if (prDrawTransformDrawsNothing(&current(w), &c)) {
            // A push still opens a scope, which its pop closes.
            if (c.kind >= PrDrawCommandKind_PushClipRect && c.kind <= PrDrawCommandKind_PushClipPath) {
                prDrawCanvasSave();
                w.clips.push(PrIRect{0, 0, 0, 0});
            } else if (c.kind == PrDrawCommandKind_PushGroup) {
                w.groups.push(Group{});
            }
            continue;
        }
        switch (c.kind) {
            case PrDrawCommandKind_Clear: {
                PrColor color = prColorFromColor16(c.color);
                prDrawCanvasClear(prF32Clamp(color.r, 0, 1), prF32Clamp(color.g, 0, 1), prF32Clamp(color.b, 0, 1),
                    prF32Clamp(color.a, 0, 1));
                break;
            }
            case PrDrawCommandKind_Border:
                drawBorder(w, c);
                break;
            case PrDrawCommandKind_DropShadow:
            case PrDrawCommandKind_InnerShadow:
                drawShadow(w, c);
                break;
            case PrDrawCommandKind_Decoration:
                drawDecoration(w, c);
                break;
            case PrDrawCommandKind_NineSlice:
                drawNineSlice(w, c);
                break;
            case PrDrawCommandKind_WritePixels:
                writePixels(w, c);
                break;
            case PrDrawCommandKind_Glyphs:
                drawGlyphs(w, c);
                break;
            case PrDrawCommandKind_Embed:
                w.transforms.push(prDrawTransformMoved(&current(w), c.embed.offset));
                runCommands(w, prDrawListCommands(c.embed.list), prDrawListCommandCount(c.embed.list));
                w.transforms.pop();
                break;
            case PrDrawCommandKind_PushTransformAffine2: {
                PrDrawTransform next = prDrawTransformPush(&current(w), &c);
                w.transforms.push(next);
                break;
            }
            case PrDrawCommandKind_PopTransform:
                w.transforms.pop();
                break;
            case PrDrawCommandKind_PushClipTargetRect:
                prPathClear(backend->path);
                prPathAddRect(backend->path, c.rect);
                pushClip(w, backend->path, prAffine2Identity(), PrPathFillRule_NonZero);
                break;
            case PrDrawCommandKind_PushClipRect:
            case PrDrawCommandKind_PushClipCornerRect:
            case PrDrawCommandKind_PushClipPath:
                prPathClear(backend->path);
                prDrawShapePath(&c, backend->path);
                pushClip(w, backend->path, current(w).affine, c.rule);
                break;
            case PrDrawCommandKind_PopClip:
                restore(w);
                w.clips.pop();
                break;
            case PrDrawCommandKind_PushGroup:
                i = pushGroup(w, commands, count, i);
                break;
            case PrDrawCommandKind_PopGroup:
                popGroup(w);
                break;
            default:
                drawShape(w, c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The renderer

// A frame: the part of its list, and the origin of each render since it was prepared. The frame's plays read the
// origins in the order that they run.
struct CanvasFrame : PrDrawFrame {
    const PrDrawList* PR_NULLABLE list;
    PrDrawPart part;
    RawArray<PrIVec2> origins;
    uint32_t played;
};

// Walks the part of the frame's list into the canvas, within the render's rectangle.
void play(void* PR_NULLABLE context, const void* native) {
    CanvasFrame* frame = static_cast<CanvasFrame*>(context);
    PrGpuCanvas canvas = *static_cast<const PrGpuCanvas*>(native);
    PrIVec2 origin = frame->origins.data[frame->played++];
    CanvasBackend* backend = static_cast<CanvasBackend*>(frame->renderer);
    if (++backend->serial % 64 == 0)
        dropGlyphs(backend);
    prDrawGlyphCacheBegin(backend->glyphCache);
    prDrawCanvasBegin(backend->pageState, canvas, origin.x, origin.y);
    RawArray<DrawnList> drawn{};
    Walk w{.backend = backend, .drawn = &drawn};
    forgetState(w);
    w.transforms.push(frame->part.transform);
    PrIRect whole{0, 0, int32_t(frame->extent.x), int32_t(frame->extent.y)};
    w.clips.push(whole);
    prDrawCanvasSave();
    prDrawCanvasRect(0, 0, float(whole.right), float(whole.bottom));
    sendTransform(prAffine2Identity());
    prDrawCanvasClip(0);
    runCommands(w, prDrawListCommands(frame->list) + frame->part.first, frame->part.count);
    // A list's pushes and pops balance, so the walk ends with the render's own clip in place.
    prDrawCanvasRestore();
    prDrawCanvasEnd();
    prDrawGlyphCacheEnd(backend->glyphCache);
    w.transforms.reset();
    w.clips.reset();
    w.groups.reset();
    drawn.reset();
}

void destroyBackend(PrDrawRenderer* renderer) {
    CanvasBackend* backend = static_cast<CanvasBackend*>(renderer);
    prDrawCanvasClose(backend->pageState);
    prDrawGlyphCacheDestroy(backend->glyphCache);
    for (CanvasGlyph& g : backend->glyphs.span())
        prPathDestroy(g.path);
    backend->glyphs.reset();
    backend->freeGlyphs.reset();
    backend->glyphIndex.reset();
    backend->operands.reset();
    backend->scratchTexels.reset();
    prPathDestroy(backend->path);
    prPathDestroy(backend->extra);
    prPathPolylineReset(&backend->fill);
    prPathPolylineReset(&backend->aligned);
    prPathPolylineReset(&backend->region);
    prMemoryFree(backend);
}

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    CanvasFrame* frame = allocZeroed<CanvasFrame>();
    frame->renderer = renderer;
    return frame;
}

void destroyFrame(PrDrawRenderer*, PrDrawFrame* frame) {
    CanvasFrame* f = static_cast<CanvasFrame*>(frame);
    f->origins.reset();
    prMemoryFree(f);
}

// Keeps the part of the list, which the frame's pass walks when it runs.
PrStatus prepare(PrDrawRenderer*, PrDrawFrame* frame, const PrDrawList* list, const PrDrawPart* part,
        const PrDrawTarget*) {
    CanvasFrame* f = static_cast<CanvasFrame*>(frame);
    f->list = list;
    f->part = *part;
    f->origins.clear();
    f->played = 0;
    return PrStatus_Ok;
}

void renderFrame(PrDrawRenderer*, PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    CanvasFrame* f = static_cast<CanvasFrame*>(frame);
    f->origins.push(origin);
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Canvas2d, play, f);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawCanvasOptions prDrawCanvasOptionsDefault(void) {
    return PrDrawCanvasOptions{};
}

PrStatus prDrawCanvasCreate(PrGpuDevice* device, const PrDrawCanvasOptions* PR_NULLABLE options,
        PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (!prGpuDeviceHasNative(device, PrGpuNativeKind_Canvas2d))
        return PrStatus_NotSupported;
    if (prGpuDeviceIsLost(device))
        return PrStatus_Lost;
    PrDrawCanvasOptions o = options != nullptr ? *options : prDrawCanvasOptionsDefault();
    CanvasBackend* backend = allocZeroed<CanvasBackend>();
    backend->pageState = prDrawCanvasOpen();
    backend->glyphCache = prDrawGlyphCacheCreate(o.glyphDecoder);
    backend->path = prPathCreate();
    backend->extra = prPathCreate();
    backend->device = device;
    backend->destroy = destroyBackend;
    backend->frameCreate = createFrame;
    backend->frameDestroy = destroyFrame;
    backend->framePrepare = prepare;
    backend->frameRender = renderFrame;
    *out = backend;
    return PrStatus_Ok;
}
