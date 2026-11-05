// The Blend2D draw backend, which renders with Blend2D into pixels in memory.
//
// The list's transforms are Blend2D's transforms. A push saves the context's state and transforms its user space, and
// its pop restores the state. Geometry goes to Blend2D in the command's space, and Blend2D fills and strokes it. A
// context's meta transform places the image that it draws into, so that its user space starts at target pixels. An
// offscreen starts with the user transform in force at its push. Blend2D clips only to rectangles, and its operators
// change only what a shape covers, so everything else uses offscreen images:
//
// - A clip of any other shape draws into an image of its bounds, then blends the image back through the clip's
// coverage. The image starts as a copy of what lies beneath if its contents read what lies beneath, and transparent
// otherwise.
// - A group with an effect draws into a transparent image, which is composited at the group's pop.
// - An operator that changes what lies outside its shape draws the shape into an image the size of the clip. The image
// is transparent outside the shape when it composites.

#include "pr/draw_blend2d.h"

#include "pr/alloc.h"
#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_projection.h"
#include "pr/draw_transform.h"
#include "pr/float.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/path_polyline.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "draw/backends/shared/src/draw_shared_memory.h"

#include "blend2d/blend2d.h"

namespace pr_impl {

namespace {

// The maximum distance of a flattened curve from the true shape, in target pixels.
constexpr float kTolerance = 0.05f;

// The colors per unit of a gradient's parameter in a ramp's pieces that mix nonlinearly.
constexpr uint32_t kRampResolution = 256;

struct CachedImage {
    uint64_t id;
    BLImageCore image;
    uint64_t lastUsed;
};

struct Blend2dBackend : PrDrawRenderer {
    bool ownsDevice;                    // true if the renderer created its device, and frees it
    PrDrawBlend2dOptions options;
    RawArray<CachedImage> images;
    KeyIndex imageIndex;                // `images`, by id
    PrDrawGlyphCache* glyphs;
    uint64_t render;
};

// An image that a render draws into, with its origin at (box.left, box.top) in target pixels. It is the target's image,
// or an offscreen for a clip, a group or an operator.
struct Target {
    BLImageCore image;
    BLContextCore context;
    PrIRect box;
};

enum {
    PushKind_Transform,     // a transform of the target's context
    PushKind_Rect,          // a rectangle clip on the target's context
    PushKind_Clip,          // a clip of any other shape: an offscreen and its coverage
    PushKind_Group,         // a group with an effect: an offscreen
    PushKind_Plain,         // a group with no effect
};
typedef uint8_t PushKind;

// A push, the state that its pop undoes, and the clip in effect inside it.
struct Push {
    PushKind kind;
    PrIRect clip;               // the device box that the clips in effect let through
    BLImageCore coverage;       // Clip: the clip's coverage, A8 over the offscreen's box
    bool beneath;               // Clip: true if the offscreen started as a copy of what lies beneath
    const PrDrawGroup* group;   // Group
    PrDrawComposite composite;  // Group
    PrDrawBlendMode blendMode;  // Group
    bool collapsed;             // Transform: true if it collapses, or is pushed inside one that does
};

// The state of one render as it walks the commands. A nested render, such as a list's render, has its own state.
struct Renderer {
    Blend2dBackend* b2;
    const PrPixels* PR_NULLABLE pixels;         // the target's pixels, Bgra8, which WritePixels writes
    RawArray<Push> pushes;
    uint32_t collapsed;                     // number of open transform pushes that are collapsed
    RawArray<Target*> targets;              // innermost last
    PrIRect clip;                           // the area that the clips in force let through
    PrPath* shape;
    PrPath* extra;
    PrPathPolyline outline;                     // a border's piece or a decoration
    PrPathPolyline combined;
    BLPathCore path;
    RawArray<PrDrawRampPiece> rampPieces;
    RawArray<PrColor> ramp;
    RawArray<BLGradientStop> stops;
};

Target& target(Renderer& r) {
    return *r.targets.last();
}

BLContextCore* context(Renderer& r) {
    return &target(r).context;
}

BLMatrix2D matrixOf(PrDAffine2 t) {
    BLMatrix2D m{};
    m.m[0] = t.a;
    m.m[1] = t.b;
    m.m[2] = t.c;
    m.m[3] = t.d;
    m.m[4] = t.e;
    m.m[5] = t.f;
    return m;
}

BLMatrix2D matrixOf(PrAffine2 t) {
    return matrixOf(prDAffine2FromAffine2(t));
}

// Returns the context's user transform, which maps the current space to target pixels.
BLMatrix2D userOf(Renderer& r) {
    BLMatrix2D m{};
    bl_context_get_user_transform(context(r), &m);
    return m;
}

// Returns the current space's transform to target pixels.
PrAffine2 toTarget(Renderer& r) {
    BLMatrix2D m = userOf(r);
    return PrAffine2{float(m.m[0]), float(m.m[1]), float(m.m[2]), float(m.m[3]), float(m.m[4]), float(m.m[5])};
}

// Saves the context's state and draws in target pixels until endDevice restores it.
void beginDevice(BLContextCore* ctx) {
    bl_context_save(ctx, nullptr);
    bl_context_apply_transform_op(ctx, BL_TRANSFORM_OP_RESET, nullptr);
}

void endDevice(BLContextCore* ctx) {
    bl_context_restore(ctx, nullptr);
}

BLRgba rgbaOf(PrColor c) {
    return BLRgba{prF32Clamp(c.r, 0, 1), prF32Clamp(c.g, 0, 1), prF32Clamp(c.b, 0, 1), prF32Clamp(c.a, 0, 1)};
}

// ---------------------------------------------------------------------------------------------------------------------
// Targets

// Begins drawing into the image, placed at the box's corner, so that the context draws in target pixels.
void beginTarget(Target* t, uint32_t threads) {
    BLContextCreateInfo info{};
    info.flags = BL_CONTEXT_CREATE_FLAG_FALLBACK_TO_SYNC;
    info.thread_count = threads;
    bl_context_init(&t->context);
    bl_context_begin(&t->context, &t->image, &info);
    double move[2] = {-double(t->box.left), -double(t->box.top)};
    bl_context_apply_transform_op(&t->context, BL_TRANSFORM_OP_TRANSLATE, move);
    bl_context_user_to_meta(&t->context);
    bl_context_set_hint(&t->context, BL_CONTEXT_HINT_GRADIENT_QUALITY, BL_GRADIENT_QUALITY_SMOOTH);
    // Curves flatten to kTolerance. Blend2D's default tolerance is several times larger.
    bl_context_set_flatten_tolerance(&t->context, double(kTolerance));
}

// Creates a transparent offscreen over the box, in the format, that draws synchronously with `user` as its user
// transform.
Target* newTarget(PrIRect box, BLFormat format, const BLMatrix2D& user) {
    Target* t = allocZeroed<Target>();
    t->box = box;
    bl_image_init(&t->image);
    uint32_t width = prIRectWidth(box) != 0 ? prIRectWidth(box) : 1;
    uint32_t height = prIRectHeight(box) != 0 ? prIRectHeight(box) : 1;
    bl_image_create(&t->image, int(width), int(height), format);
    beginTarget(t, 0);
    bl_context_clear_all(&t->context);
    bl_context_apply_transform_op(&t->context, BL_TRANSFORM_OP_ASSIGN, &user);
    return t;
}

// Ends drawing into the target, keeping its image.
void endTarget(Target* t) {
    bl_context_end(&t->context);
    bl_context_destroy(&t->context);
}

void destroyTarget(Target* t) {
    bl_image_destroy(&t->image);
    prMemoryFree(t);
}

// Makes a pattern of a target's image, placed at its position on the device.
void targetPattern(const Target* t, BLPatternCore* out) {
    BLMatrix2D place = matrixOf(prAffine2Translate(PrVec2{float(t->box.left), float(t->box.top)}));
    bl_pattern_init_as(out, &t->image, nullptr, BL_EXTEND_MODE_PAD, &place);
}

// Prepares the target for an offscreen to composite into it. Blend2D's asynchronous rendering has crashed when
// compositing an offscreen after queued fills under blend modes. An asynchronous target therefore finishes its queued
// work first.
void readyForOffscreen(Target* t) {
    bl_context_flush(&t->context, BL_CONTEXT_FLUSH_SYNC);
}

// ---------------------------------------------------------------------------------------------------------------------
// Paths

// Converts the path to a Blend2D path.
void pathOf(Renderer& r, const PrPath* path) {
    bl_path_clear(&r.path);
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t next = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move: {
                PrVec2 p = points[next++];
                bl_path_move_to(&r.path, double(p.x), double(p.y));
                break;
            }
            case PrPathVerb_Line: {
                PrVec2 p = points[next++];
                bl_path_line_to(&r.path, double(p.x), double(p.y));
                break;
            }
            case PrPathVerb_Quad: {
                PrVec2 c = points[next];
                PrVec2 p = points[next + 1];
                next += 2;
                bl_path_quad_to(&r.path, double(c.x), double(c.y), double(p.x), double(p.y));
                break;
            }
            case PrPathVerb_Cubic: {
                PrVec2 c1 = points[next];
                PrVec2 c2 = points[next + 1];
                PrVec2 p = points[next + 2];
                next += 3;
                bl_path_cubic_to(&r.path, double(c1.x), double(c1.y), double(c2.x), double(c2.y), double(p.x),
                    double(p.y));
                break;
            }
            case PrPathVerb_Close:
                bl_path_close(&r.path);
                break;
            default:
                break;
        }
    }
}

// Converts the polyline to a Blend2D path.
void pathOf(Renderer& r, const PrPathPolyline& polyline) {
    bl_path_clear(&r.path);
    for (uint32_t c = 0; c < polyline.contourCount; ++c) {
        uint32_t start = polyline.contours[c];
        uint32_t end = prPathPolylineContourEnd(&polyline, c);
        if (end == start)
            continue;
        bl_path_move_to(&r.path, double(polyline.points[start].x), double(polyline.points[start].y));
        for (uint32_t i = start + 1; i < end; ++i)
            bl_path_line_to(&r.path, double(polyline.points[i].x), double(polyline.points[i].y));
        bl_path_close(&r.path);
    }
}

// A shape in the current space: a path, or a polyline.
struct Shape {
    const PrPath* PR_NULLABLE path;
    const PrPathPolyline* PR_NULLABLE polyline;
};

void pathOf(Renderer& r, const Shape& shape) {
    if (shape.path != nullptr)
        pathOf(r, shape.path);
    else
        pathOf(r, *shape.polyline);
}

bool shapeEmpty(const Shape& shape) {
    return shape.path != nullptr ? prPathVerbCount(shape.path) == 0 : shape.polyline->pointCount == 0;
}

// Returns the device box that a rectangle of a space covers, where `x` maps the space to target pixels.
PrIRect deviceBox(PrAffine2 x, PrRect rect) {
    return prIRectRoundOut(prAffine2MapRect(x, rect));
}

// Returns the device box that the shape covers, where `x` maps its space to target pixels.
PrIRect shapeBox(const Shape& shape, PrAffine2 x) {
    if (shapeEmpty(shape))
        return PrIRect{0, 0, 0, 0};
    return deviceBox(x, shape.path != nullptr ? prPathBounds(shape.path) : prPathPolylineBounds(shape.polyline));
}

// Returns true if the path, mapped by `x`, is one axis-aligned rectangle with its edges on whole pixels, and writes the
// rectangle. Blend2D clips to a rectangle that is off whole pixels, but composites a mask through such a clip as
// nothing.
bool deviceRect(const PrPath* path, PrAffine2 x, PrRect* out) {
    PrVec2 p[4];
    const PrPathVerb* verbs = prPathVerbs(path);
    if (prPathVerbCount(path) != 5 || verbs[0] != PrPathVerb_Move || verbs[4] != PrPathVerb_Close)
        return false;
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != 0 && verbs[i] != PrPathVerb_Line)
            return false;
        p[i] = prAffine2MapPoint(x, prPathPoints(path)[i]);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        PrVec2 a = p[i];
        PrVec2 b = p[(i + 1) % 4];
        if (a.x != b.x && a.y != b.y)
            return false;
    }
    *out = PrRect{p[0].x, p[0].y, p[0].x, p[0].y};
    for (const PrVec2& q : p) {
        out->left = prF32Min(out->left, q.x);
        out->top = prF32Min(out->top, q.y);
        out->right = prF32Max(out->right, q.x);
        out->bottom = prF32Max(out->bottom, q.y);
    }
    float edges[4] = {out->left, out->top, out->right, out->bottom};
    for (float e : edges) {
        if (e != prF32Floor(e))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Compositing

// Returns Blend2D's operator for ours. Without its JIT, Blend2D's portable pipelines draw only the operators kept here,
// and leave the destination unchanged for the rest. Every other operator and every blend mode therefore draws as
// source-over.
BLCompOp compOpOf(PrDrawComposite composite) {
    switch (composite) {
        case PrDrawComposite_Copy:
            return BL_COMP_OP_SRC_COPY;
        case PrDrawComposite_Clear:
            return BL_COMP_OP_CLEAR;
        case PrDrawComposite_DestinationOut:
            return BL_COMP_OP_DST_OUT;
        default:
            return BL_COMP_OP_SRC_OVER;
    }
}

// Begins and ends a painting command's compositing. Blend2D applies the operator where the shape covers, which is where
// a command's operator applies.
void beginPaint(Renderer& r, const PrDrawCommand& c) {
    bl_context_set_comp_op(context(r), compOpOf(c.composite));
}

void endPaint(Renderer& r, const PrDrawCommand&) {
    bl_context_set_comp_op(context(r), BL_COMP_OP_SRC_OVER);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Returns a Blend2D image of the image's pixels, premultiplied BGRA. It is created once and kept for as long as renders
// use it.
const BLImageCore* imageOf(Blend2dBackend* b2, const PrDrawImage* image) {
    uint64_t id = prDrawImageId(image);
    uint32_t found = b2->imageIndex.find(id, [&](uint32_t i) { return b2->images.data[i].id; });
    if (found != kHashMissing) {
        b2->images.data[found].lastUsed = b2->render;
        return &b2->images.data[found].image;
    }
    uint32_t index = b2->images.count;
    CachedImage& cached = b2->images.emplace();
    cached = CachedImage{id, {}, b2->render};
    bl_image_init(&cached.image);
    uint32_t width = prDrawImageExtent(image).x;
    uint32_t height = prDrawImageExtent(image).y;
    bl_image_create(&cached.image, int(width), int(height), BL_FORMAT_PRGB32);
    BLImageData data{};
    bl_image_make_mutable(&cached.image, &data);
    PrPixels pixels{static_cast<uint8_t*>(data.pixel_data), size_t(data.stride), {width, height}};
    prDrawImageWritePixels(image, pixels, PrPixelFormat_Bgra8, PrIVec2{0, 0});
    b2->imageIndex.insert(id, index);
    return &cached.image;
}

// Returns the image of an image source, or null for a list source.
const BLImageCore* PR_NULLABLE sourceImage(Renderer& r, const PrDrawSource& source) {
    return source.kind == PrDrawSourceKind_Image ? imageOf(r.b2, source.image) : nullptr;
}

// Returns Blend2D's extend mode for a tile mode on each axis. None extends in the same way as Pad.
BLExtendMode extendOf(PrDrawExtend x, PrDrawExtend y) {
    auto simple = [](PrDrawExtend m) {
        return m == PrDrawExtend_Repeat ? 1u : m == PrDrawExtend_Reflect ? 2u : 0u;
    };
    // Rows by x's mode, and columns by y's mode: pad, repeat, reflect.
    constexpr BLExtendMode kModes[3][3] = {
        {BL_EXTEND_MODE_PAD_X_PAD_Y, BL_EXTEND_MODE_PAD_X_REPEAT_Y, BL_EXTEND_MODE_PAD_X_REFLECT_Y},
        {BL_EXTEND_MODE_REPEAT_X_PAD_Y, BL_EXTEND_MODE_REPEAT_X_REPEAT_Y, BL_EXTEND_MODE_REPEAT_X_REFLECT_Y},
        {BL_EXTEND_MODE_REFLECT_X_PAD_Y, BL_EXTEND_MODE_REFLECT_X_REPEAT_Y, BL_EXTEND_MODE_REFLECT_X_REFLECT_Y},
    };
    return kModes[simple(x)][simple(y)];
}

// Makes a pattern of the image's `texels` mapped onto `onto`, in the space that `toSpace` maps to the style's space.
// Texels on whole pixels become the pattern's area, which repeats. Otherwise the pattern uses the whole image, placed
// so that the texels land in the right place.
void patternOf(const BLImageCore* image, PrRect texels, PrRect onto, PrAffine2 toSpace,
        BLExtendMode extend, BLPatternCore* out) {
    bool whole = texels.left == prF32Floor(texels.left) && texels.top == prF32Floor(texels.top) &&
        texels.right == prF32Floor(texels.right) && texels.bottom == prF32Floor(texels.bottom);
    float sx = prRectWidth(onto) / prRectWidth(texels);
    float sy = prRectHeight(onto) / prRectHeight(texels);
    PrVec2 origin = whole ? PrVec2{0, 0} : PrVec2{texels.left, texels.top};
    PrAffine2 place = prAffine2Concat(prAffine2Concat(prAffine2Translate(-origin), prAffine2Scale(PrVec2{sx, sy})),
        prAffine2Translate(PrVec2{onto.left, onto.top}));
    BLMatrix2D m = matrixOf(prAffine2Concat(place, toSpace));
    if (whole) {
        BLRectI area{int(texels.left), int(texels.top), int(prRectWidth(texels)), int(prRectHeight(texels))};
        bl_pattern_init_as(out, image, &area, extend, &m);
    } else {
        bl_pattern_init_as(out, image, nullptr, extend, &m);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// Computes a gradient's stops into `r.stops`: the ramp's colors, with straight alpha, which Blend2D expects. Each
// piece's colors are stops at their place in the piece, so that pieces that meet with different colors are two stops at
// one offset.
void gradientStops(Renderer& r, const PrDrawBrush* brush, PrColor color) {
    r.rampPieces.resize(prDrawGradientRampMaxPieces(brush->stopCount));
    r.ramp.resize(prDrawGradientRampMaxColors(brush->stopCount, kRampResolution));
    r.rampPieces.resize(prDrawGradientRamp(brush, color, PrColorSpace_Srgb, kRampResolution, r.rampPieces.data,
        r.ramp.data));
    r.stops.clear();
    // The pieces past the ends are left out: Blend2D extends the end stops itself.
    for (uint32_t p = 1; p + 1 < r.rampPieces.count; ++p) {
        const PrDrawRampPiece& piece = r.rampPieces.data[p];
        // A piece of one color is a stop at each end.
        uint32_t ends = piece.count == 1 ? 2 : piece.count;
        for (uint32_t k = 0; k < ends; ++k) {
            const PrColor& c = r.ramp.data[piece.first + (piece.count == 1 ? 0 : k)];
            float a = prF32Clamp(c.a, 0, 1);
            auto channel = [&](float v) {
                return uint64_t(prF32Clamp(a > 0 ? v / a : 0, 0, 1) * 65535 + 0.5f);
            };
            BLGradientStop stop{};
            stop.offset = double(piece.start) + double(piece.end - piece.start) * double(k) / double(ends - 1);
            stop.rgba.value =
                (uint64_t(a * 65535 + 0.5f) << 48) | (channel(c.r) << 32) | (channel(c.g) << 16) | channel(c.b);
            r.stops.push(stop);
        }
    }
}

// Computes the transform from the brush's space to the current space, or to target pixels for a Target brush, and
// whether it maps to target pixels. Returns false if the brush paints nothing.
bool brushPlacement(const PrDrawBrush* brush, PrRect bounds, PrAffine2* toSpace, bool* inTarget) {
    PrAffine2 fromSpace{};
    *inTarget = brush->space == PrDrawBrushSpace_Target;
    return prDrawBrushFromTarget(brush, prAffine2Identity(), bounds, &fromSpace) && prAffine2Invert(fromSpace, toSpace);
}

// Sets the target's fill or stroke style to the paint: the color, or the brush times the color, for a command whose
// fill geometry has `bounds` in the current space. The caller resets the style with resetStyle. Returns false if it
// paints nothing.
bool setStyle(Renderer& r, BLContextStyleSlot slot, const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds) {
    BLContextCore* ctx = context(r);
    bool fill = slot == BL_CONTEXT_STYLE_SLOT_FILL;
    if (brush == nullptr) {
        BLRgba rgba = rgbaOf(color);
        if (fill)
            bl_context_set_fill_style_rgba(ctx, &rgba);
        else
            bl_context_set_stroke_style_rgba(ctx, &rgba);
        return true;
    }
    PrAffine2 toSpace{};
    bool inTarget = false;
    if (!brushPlacement(brush, bounds, &toSpace, &inTarget))
        return false;
    PrAffine2 toDevice = inTarget ? toSpace : prAffine2Concat(toSpace, toTarget(r));
    // A Target brush's style is in target pixels, which the meta transform maps to the image.
    BLContextStyleTransformMode mode =
        inTarget ? BL_CONTEXT_STYLE_TRANSFORM_MODE_META : BL_CONTEXT_STYLE_TRANSFORM_MODE_USER;
    auto setPaint = [&](const void* style) {
        if (fill)
            bl_context_set_fill_style_with_mode(ctx, style, mode);
        else
            bl_context_set_stroke_style_with_mode(ctx, style, mode);
    };
    if (brush->kind == PrDrawBrushKind_Image) {
        // A list source paints nothing.
        const PrDrawSource& source = brush->image.source;
        const PrDrawTiles& tiles = brush->image.tiles;
        const BLImageCore* image = sourceImage(r, source);
        PrRect texels = source.rect;
        if (image == nullptr || prRectIsEmpty(tiles.tile) || prRectIsEmpty(texels))
            return false;
        BLPatternCore pattern{};
        patternOf(image, texels, tiles.tile, toSpace, extendOf(tiles.x, tiles.y), &pattern);
        bool nearest = (brush->image.sampling & PrDrawSamplingFlag_Kind) == PrDrawSampling_Nearest;
        bl_context_set_hint(ctx, BL_CONTEXT_HINT_PATTERN_QUALITY,
            nearest ? BL_PATTERN_QUALITY_NEAREST : BL_PATTERN_QUALITY_BILINEAR);
        setPaint(&pattern);
        bl_pattern_destroy(&pattern);
        double alpha = double(prF32Clamp(color.a, 0, 1));
        if (fill)
            bl_context_set_fill_alpha(ctx, alpha);
        else
            bl_context_set_stroke_alpha(ctx, alpha);
        return true;
    }
    gradientStops(r, brush, color);
    const BLGradientStop* stops = r.stops.data;
    const size_t stopCount = r.stops.count;
    BLExtendMode extend = BL_EXTEND_MODE_PAD;
    if (brush->extend == PrDrawExtend_Repeat)
        extend = BL_EXTEND_MODE_REPEAT;
    else if (brush->extend == PrDrawExtend_Reflect)
        extend = BL_EXTEND_MODE_REFLECT;
    BLGradientCore gradient{};
    BLMatrix2D m = matrixOf(toSpace);
    switch (brush->kind) {
        case PrDrawBrushKind_Linear: {
            BLLinearGradientValues v{};
            v.x0 = double(brush->linear.start.x);
            v.y0 = double(brush->linear.start.y);
            v.x1 = double(brush->linear.end.x);
            v.y1 = double(brush->linear.end.y);
            bl_gradient_init_as(&gradient, BL_GRADIENT_TYPE_LINEAR, &v, extend, stops, stopCount, &m);
            break;
        }
        case PrDrawBrushKind_Radial: {
            // Uses units the size of a target pixel. Blend2D moves a focal point that is within half a unit of the
            // outer circle. It divides by zero if the focal point is the center and the radius is below that half unit.
            float k = prAffine2MaxScale(toDevice);
            if (!(k > 0))
                return false;
            BLMatrix2D scaled = matrixOf(prAffine2Concat(prAffine2Scale(PrVec2{1 / k, 1 / k}), toSpace));
            BLRadialGradientValues v{};
            v.x0 = double(brush->radial.end.x * k);
            v.y0 = double(brush->radial.end.y * k);
            v.x1 = double(brush->radial.start.x * k);
            v.y1 = double(brush->radial.start.y * k);
            v.r0 = double(brush->radial.endRadius * k);
            v.r1 = double(brush->radial.startRadius * k);
            bl_gradient_init_as(&gradient, BL_GRADIENT_TYPE_RADIAL, &v, extend, stops, stopCount, &scaled);
            break;
        }
        case PrDrawBrushKind_Conic: {
            BLConicGradientValues v{};
            v.x0 = double(brush->conic.center.x);
            v.y0 = double(brush->conic.center.y);
            v.angle = double(brush->conic.angle);
            v.repeat = 1;
            bl_gradient_init_as(&gradient, BL_GRADIENT_TYPE_CONIC, &v, extend, stops, stopCount, &m);
            break;
        }
        default: {
            // A diamond is approximated by the ellipse through its corners.
            const PrDrawDiamondGradient& g = brush->diamond;
            if (!(g.radii.x > 0 && g.radii.y > 0))
                return false;
            // A unit circle scaled to the radii, 256 units wide for the reason above.
            BLMatrix2D ellipse = matrixOf(prAffine2Concat(prAffine2Concat(prAffine2Scale(g.radii / 256),
                prAffine2Translate(g.center)), toSpace));
            BLRadialGradientValues v{};
            v.r0 = 256;
            bl_gradient_init_as(&gradient, BL_GRADIENT_TYPE_RADIAL, &v, extend, stops, stopCount, &ellipse);
            break;
        }
    }
    setPaint(&gradient);
    bl_gradient_destroy(&gradient);
    return true;
}

void resetStyle(Renderer& r) {
    bl_context_set_fill_alpha(context(r), 1);
    bl_context_set_stroke_alpha(context(r), 1);
}

// Returns the tolerance in the current space that flattens within kTolerance target pixels, or zero if the space
// collapses.
float spaceTolerance(Renderer& r) {
    float scale = prAffine2MaxScale(toTarget(r));
    return scale > 0 ? kTolerance / scale : 0;
}

// Fills the shape with the paint.
void fillShape(Renderer& r, const Shape& shape, PrPathFillRule rule, const PrDrawBrush* PR_NULLABLE brush,
        PrColor color, PrRect bounds) {
    if (shapeEmpty(shape) || !setStyle(r, BL_CONTEXT_STYLE_SLOT_FILL, brush, color, bounds))
        return;
    pathOf(r, shape);
    BLContextCore* ctx = context(r);
    bl_context_set_fill_rule(ctx, rule == PrPathFillRule_EvenOdd ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO);
    BLPoint origin{0, 0};
    bl_context_fill_path_d(ctx, &origin, &r.path);
    resetStyle(r);
}

void fillPolyline(Renderer& r, const PrPathPolyline& polyline, PrPathFillRule rule,
        const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds) {
    fillShape(r, Shape{nullptr, &polyline}, rule, brush, color, bounds);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

BLStrokeJoin joinOf(PrPathJoin join) {
    switch (join) {
        case PrPathJoin_MiterClip:
        case PrPathJoin_Arcs:
            return BL_STROKE_JOIN_MITER_CLIP;
        case PrPathJoin_Round:
            return BL_STROKE_JOIN_ROUND;
        case PrPathJoin_Bevel:
            return BL_STROKE_JOIN_BEVEL;
        default:
            return BL_STROKE_JOIN_MITER_BEVEL;
    }
}

BLStrokeCap capOf(PrPathCap cap) {
    switch (cap) {
        case PrPathCap_Round:
            return BL_STROKE_CAP_ROUND;
        case PrPathCap_Square:
            return BL_STROKE_CAP_SQUARE;
        case PrPathCap_Triangle:
            return BL_STROKE_CAP_TRIANGLE;
        default:
            return BL_STROKE_CAP_BUTT;
    }
}

// Fills or strokes `r.shape`, in the command's space, with its paint. A non-scaling stroke is measured in device
// pixels.
void drawShapePath(Renderer& r, const PrDrawCommand& c, PrRect bounds) {
    if (c.stroke == nullptr) {
        fillShape(r, Shape{r.shape, nullptr}, c.rule, c.brush, c.color, bounds);
        return;
    }
    const PrPathStroke& stroke = c.stroke->shape;
    if (!(stroke.width > 0) || prPathVerbCount(r.shape) == 0 ||
            !setStyle(r, BL_CONTEXT_STYLE_SLOT_STROKE, c.brush, c.color, bounds))
        return;
    BLContextCore* ctx = context(r);
    bl_context_set_stroke_width(ctx, double(stroke.width));
    bl_context_set_stroke_miter_limit(ctx, double(stroke.miterLimit));
    bl_context_set_stroke_join(ctx, joinOf(stroke.join));
    bl_context_set_stroke_cap(ctx, BL_STROKE_CAP_POSITION_START, capOf(stroke.startCap));
    bl_context_set_stroke_cap(ctx, BL_STROKE_CAP_POSITION_END, capOf(stroke.endCap));
    bl_context_set_stroke_transform_order(ctx,
        c.stroke->nonScaling ? BL_STROKE_TRANSFORM_ORDER_BEFORE : BL_STROKE_TRANSFORM_ORDER_AFTER);
    pathOf(r, r.shape);
    BLPoint origin{0, 0};
    bl_context_stroke_path_d(ctx, &origin, &r.path);
    resetStyle(r);
}

void drawShape(Renderer& r, const PrDrawCommand& c) {
    if (c.kind == PrDrawCommandKind_Line && c.stroke == nullptr)
        return;
    beginPaint(r, c);
    prPathClear(r.shape);
    prDrawShapePath(&c, r.shape);
    drawShapePath(r, c, prDrawFillBounds(&c));
    endPaint(r, c);
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

struct BorderDraw {
    Renderer* r;
    const PrDrawCommand* command;
};

void drawBorderPiece(void* PR_NULLABLE context, const PrDrawBorderPiece* piece,
        const PrPathPolyline* shape, const PrPathPolyline* PR_NULLABLE cut) {
    BorderDraw& d = *static_cast<BorderDraw*>(context);
    Renderer& r = *d.r;
    const PrDrawCommand& c = *d.command;
    const PrDrawBorderSide& side = c.border->sides[piece->side];
    if (cut == nullptr) {
        fillPolyline(r, *shape, piece->rule, side.brush, piece->color, c.border->shape.rect);
        return;
    }
    prPathPolylineClear(&r.combined);
    prPathPolylineCombine(shape, piece->rule, cut, PrPathFillRule_NonZero, PrPathPolylineOp_Intersect, &r.combined);
    fillPolyline(r, r.combined, PrPathFillRule_NonZero, side.brush, piece->color, c.border->shape.rect);
}

void drawBorder(Renderer& r, const PrDrawCommand& c) {
    float tolerance = spaceTolerance(r);
    if (!(tolerance > 0))
        return;
    beginPaint(r, c);
    BorderDraw d{&r, &c};
    prDrawBorderLower(c.border, tolerance, drawBorderPiece, &d);
    endPaint(r, c);
}

// Computes the coverage of the shape over the box, as an A8 image, where `user` maps the shape's space to target
// pixels.
void coverageOf(Renderer& r, const Shape& shape, PrPathFillRule rule, PrIRect box, const BLMatrix2D& user, BLImageCore* out) {
    Target* t = newTarget(box, BL_FORMAT_A8, user);
    r.targets.push(t);
    BLRgba white{1, 1, 1, 1};
    bl_context_set_fill_style_rgba(&t->context, &white);
    if (!shapeEmpty(shape)) {
        pathOf(r, shape);
        bl_context_set_fill_rule(&t->context,
            rule == PrPathFillRule_EvenOdd ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO);
        BLPoint origin{0, 0};
        bl_context_fill_path_d(&t->context, &origin, &r.path);
    }
    r.targets.pop();
    endTarget(t);
    *out = t->image;
    prMemoryFree(t);
}

void drawDecoration(Renderer& r, const PrDrawCommand& c) {
    float tolerance = spaceTolerance(r);
    if (!(tolerance > 0))
        return;
    beginPaint(r, c);
    prPathPolylineClear(&r.outline);
    prDrawDecorationOutline(c.decoration, tolerance, &r.outline);
    fillPolyline(r, r.outline, PrPathFillRule_NonZero, c.brush, c.color, prDrawFillBounds(&c));
    endPaint(r, c);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Draws rectangles of the command's source, measured from the corner of its rect. Each rectangle maps its source texels
// onto its destination, with the color's alpha. A list source draws nothing.
void drawImageRects(Renderer& r, const PrDrawCommand& c, const PrDrawImageRect* rects, uint32_t count) {
    const PrDrawSource& source = c.nineSlice.source;
    const BLImageCore* image = sourceImage(r, source);
    if (image == nullptr)
        return;
    const PrRect& texels = source.rect;
    beginPaint(r, c);
    BLContextCore* ctx = context(r);
    bool nearest = (c.sampling & PrDrawSamplingFlag_Kind) == PrDrawSampling_Nearest;
    bl_context_set_hint(ctx, BL_CONTEXT_HINT_PATTERN_QUALITY,
        nearest ? BL_PATTERN_QUALITY_NEAREST : BL_PATTERN_QUALITY_BILINEAR);
    for (uint32_t i = 0; i < count; ++i) {
        const PrRect& s = rects[i].source;
        PrRect from{texels.left + s.left, texels.top + s.top, texels.left + s.right, texels.top + s.bottom};
        const PrRect& to = rects[i].destination;
        if (prRectIsEmpty(from) || prRectIsEmpty(to))
            continue;
        BLPatternCore pattern{};
        patternOf(image, from, to, prAffine2Identity(), BL_EXTEND_MODE_PAD, &pattern);
        bl_context_set_fill_style(ctx, &pattern);
        bl_pattern_destroy(&pattern);
        bl_context_set_fill_alpha(ctx, double(prF32Clamp(prFloat16ToFloat(c.color.a), 0, 1)));
        prPathClear(r.shape);
        prPathAddRect(r.shape, to);
        pathOf(r, r.shape);
        bl_context_set_fill_rule(ctx, BL_FILL_RULE_NON_ZERO);
        BLPoint origin{0, 0};
        bl_context_fill_path_d(ctx, &origin, &r.path);
        resetStyle(r);
    }
    endPaint(r, c);
}

void drawNineSlice(Renderer& r, const PrDrawCommand& c) {
    const PrRect& area = c.nineSlice.source.rect;
    float width = prRectWidth(area);
    float height = prRectHeight(area);
    uint32_t count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{width, height}, c.nineSlice.destination, nullptr, 0);
    RawArray<PrDrawImageRect> pieces;
    pieces.reserve(count);
    pieces.count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{width, height}, c.nineSlice.destination, pieces.data, count);
    drawImageRects(r, c, pieces.data, pieces.count);
    pieces.reset();
}

// Replaces the target's pixels at the device point with the image's, ignoring all state.
void writePixels(Renderer& r, const PrDrawCommand& c) {
    if (r.pixels == nullptr)
        return;
    bl_context_flush(&r.targets.data[0]->context, BL_CONTEXT_FLUSH_SYNC);
    prDrawImageWritePixels(c.writePixels.image, *r.pixels, PrPixelFormat_Bgra8, c.writePixels.origin);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

void runCommands(Renderer& r, const PrDrawCommand* commands, uint32_t count);

// Plays a color glyph's list at the glyph.
void drawColorGlyph(Renderer& r, const PrDrawCommand& c, PrDrawList* drawn, uint32_t index) {
    PrDrawGroup group{};
    PrDrawCommand commands[kPrDrawGlyphMaxCommands];
    runCommands(r, commands, prDrawGlyphListCommands(&c, index, drawn, &group, commands));
}

// Fills the outline of each glyph separately, and draws a color glyph with its layers.
void drawGlyphs(Renderer& r, const PrDrawCommand& c) {
    const PrFontGlyphRun* run = c.glyphs;
    if (c.stroke != nullptr) {
        drawShape(r, c);
        return;
    }
    PrRect bounds = prDrawFillBounds(&c);
    PrColor foreground = c.color;
    foreground.a = 1;
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        uint16_t glyph = run->glyphs[i].id;
        float pixelsPerEm = prDrawGlyphPixelsPerEm(run, i, toTarget(r));
        PrDrawList* drawn =
            prDrawGlyphCacheList(r.b2->glyphs, run->font, glyph, pixelsPerEm, &run->palette, foreground);
        if (drawn != nullptr) {
            drawColorGlyph(r, c, drawn, i);
            continue;
        }
        PrAffine2 x = toTarget(r);
        prPathClear(r.shape);
        if (!prDrawGlyphPath(run, i, &x, r.shape))
            continue;
        beginPaint(r, c);
        fillShape(r, Shape{r.shape, nullptr}, PrPathFillRule_NonZero, c.brush, c.color, bounds);
        endPaint(r, c);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Clips and groups

// Returns true if any of the commands, including those of played lists, reads what lies beneath it: an operator other
// than source-over, a blend mode other than normal, or a clear. If none does, a clip's offscreen starts transparent.
bool readsBeneath(const PrDrawCommand* commands, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = commands[i];
        switch (c.kind) {
            case PrDrawCommandKind_Clear:
                return true;
            case PrDrawCommandKind_Embed:
                if (readsBeneath(prDrawListCommands(c.embed.list), prDrawListCommandCount(c.embed.list)))
                    return true;
                break;
            case PrDrawCommandKind_PushGroup:
                if (c.group->composite != PrDrawComposite_SourceOver ||
                        c.group->blendMode != PrDrawBlendMode_Normal || c.group->backdrop != nullptr)
                    return true;
                break;
            default:
                if (c.composite != PrDrawComposite_SourceOver || c.blendMode != PrDrawBlendMode_Normal)
                    return true;
                break;
        }
    }
    return false;
}

// Returns the number of commands from `from` to the pop that closes the push at `from - 1`.
uint32_t pushedCount(const PrDrawCommand* commands, uint32_t count, uint32_t from) {
    uint32_t depth = 1;
    for (uint32_t i = from; i < count; ++i) {
        switch (commands[i].kind) {
            case PrDrawCommandKind_PushTransformAffine2:
            case PrDrawCommandKind_PushClipRect:
            case PrDrawCommandKind_PushClipTargetRect:
            case PrDrawCommandKind_PushClipCornerRect:
            case PrDrawCommandKind_PushClipPath:
            case PrDrawCommandKind_PushGroup:
                ++depth;
                break;
            case PrDrawCommandKind_PopTransform:
            case PrDrawCommandKind_PopClip:
            case PrDrawCommandKind_PopGroup:
                if (--depth == 0)
                    return i - from;
                break;
            default:
                break;
        }
    }
    return count - from;
}

// Copies what lies beneath into a clip's offscreen, which starts as that copy.
void copyBeneath(Renderer& r, Target* layer) {
    Target& parent = target(r);
    bl_context_flush(&parent.context, BL_CONTEXT_FLUSH_SYNC);
    beginDevice(&layer->context);
    bl_context_set_comp_op(&layer->context, BL_COMP_OP_SRC_COPY);
    BLPoint origin{double(parent.box.left), double(parent.box.top)};
    bl_context_blit_image_d(&layer->context, &origin, &parent.image, nullptr);
    endDevice(&layer->context);
}

// Pushes a clip. A rectangle on whole target pixels is set on the target's context. Any other shape becomes an
// offscreen over its bounds, with its coverage there.
void pushClip(Renderer& r, const PrDrawCommand* commands, uint32_t count, uint32_t at) {
    const PrDrawCommand& c = commands[at];
    Push push{};
    push.kind = PushKind_Rect;
    // A target rectangle is in target pixels.
    bool inTarget = c.kind == PrDrawCommandKind_PushClipTargetRect;
    prPathClear(r.shape);
    if (inTarget)
        prPathAddRect(r.shape, c.rect);
    else
        prDrawShapePath(&c, r.shape);
    PrAffine2 x = inTarget ? prAffine2Identity() : toTarget(r);
    BLContextCore* ctx = context(r);
    PrRect rect{};
    if (r.collapsed != 0 || prPathVerbCount(r.shape) == 0) {
        push.clip = PrIRect{0, 0, 0, 0};
        bl_context_save(ctx, nullptr);
        BLRect none{0, 0, 0, 0};
        bl_context_clip_to_rect_d(ctx, &none);
    } else if (deviceRect(r.shape, x, &rect)) {
        push.clip = prIRectIntersect(r.clip, prIRectRoundOut(rect));
        bl_context_save(ctx, nullptr);
        // Clips in target pixels, then restores the user transform in force.
        BLMatrix2D user = userOf(r);
        bl_context_apply_transform_op(ctx, BL_TRANSFORM_OP_RESET, nullptr);
        BLRect clip{double(rect.left), double(rect.top), double(prRectWidth(rect)), double(prRectHeight(rect))};
        bl_context_clip_to_rect_d(ctx, &clip);
        bl_context_apply_transform_op(ctx, BL_TRANSFORM_OP_ASSIGN, &user);
    } else {
        push.kind = PushKind_Clip;
        Shape shape{r.shape, nullptr};
        PrIRect box = prIRectIntersect(r.clip, shapeBox(shape, x));
        push.clip = box;
        BLMatrix2D user = userOf(r);
        BLMatrix2D identity{};
        bl_matrix2d_set_identity(&identity);
        coverageOf(r, shape, c.rule, box, inTarget ? identity : user, &push.coverage);
        push.beneath = readsBeneath(commands + at + 1, pushedCount(commands, count, at + 1));
        Target* layer = newTarget(box, BL_FORMAT_PRGB32, user);
        if (push.beneath && !prIRectIsEmpty(box))
            copyBeneath(r, layer);
        r.targets.push(layer);
    }
    r.pushes.push(push);
    r.clip = push.clip;
}

// Computes the group's mask over the offscreen's box as an A8 image: its brush times its color, which Blend2D draws
// into the image. An inverted mask is full coverage cleared through that image. A luminance mask is read as alpha. The
// group's space is the current one.
void maskOf(Renderer& r, const Push& push, const PrIRect& box, BLImageCore* out) {
    const PrDrawMask* mask = push.group->mask;
    PrRect bounds = push.group->bounds;
    PrAffine2 fromTarget{};
    if (prRectIsEmpty(bounds) && prAffine2Invert(toTarget(r), &fromTarget))
        bounds = prAffine2MapRect(fromTarget,
            PrRect{float(box.left), float(box.top), float(box.right), float(box.bottom)});
    Target* painted = newTarget(box, BL_FORMAT_A8, userOf(r));
    r.targets.push(painted);
    if (setStyle(r, BL_CONTEXT_STYLE_SLOT_FILL, &mask->brush, PrColor{1, 1, 1, mask->brush.opacity}, bounds))
        bl_context_fill_all(&painted->context);
    r.targets.pop();
    endTarget(painted);
    if (mask->invert) {
        BLMatrix2D identity{};
        bl_matrix2d_set_identity(&identity);
        Target* inverse = newTarget(box, BL_FORMAT_A8, identity);
        BLRgba white{1, 1, 1, 1};
        bl_context_set_fill_style_rgba(&inverse->context, &white);
        bl_context_fill_all(&inverse->context);
        bl_context_set_comp_op(&inverse->context, BL_COMP_OP_CLEAR);
        BLPoint origin{double(box.left), double(box.top)};
        bl_context_fill_mask_d(&inverse->context, &origin, &painted->image, nullptr);
        endTarget(inverse);
        destroyTarget(painted);
        painted = inverse;
    }
    *out = painted->image;
    prMemoryFree(painted);
}

// Pushes a projected group at `at` as two pushes: a clip to the quad where its plane lands, and inside it the group's
// offscreen, whose user transform is the projection's tangent at the center of the plane's visible part. The clip's
// offscreen has the same user transform, so that the group's mask is in the plane. A group that draws nothing is a clip
// to nothing.
void pushProjectedGroup(Renderer& r, const PrDrawCommand* commands, uint32_t at) {
    const PrDrawCommand& c = commands[at];
    const PrDrawGroup* g = c.group;
    PrDrawProjection p{};
    if (r.collapsed != 0 || prIRectIsEmpty(r.clip) ||
            !prDrawProjectionPlace(g->projection, toTarget(r), g->bounds, prRectFromIRect(r.clip),
                float(kPrDrawRenderMaxSize), &p)) {
        Push nothing{};
        nothing.kind = PushKind_Rect;
        bl_context_save(context(r), nullptr);
        BLRect none{0, 0, 0, 0};
        bl_context_clip_to_rect_d(context(r), &none);
        r.pushes.push(nothing);
        r.clip = nothing.clip;
        return;
    }
    prPathPolylineClear(&r.outline);
    prDrawProjectionQuad(&p, prAffine2MapRect(prAffine2Scale(PrVec2{p.scale, p.scale}), p.area), &r.outline);
    Shape quad{nullptr, &r.outline};
    BLMatrix2D identity{};
    bl_matrix2d_set_identity(&identity);
    BLMatrix2D tangent = matrixOf(prDrawProjectionTangent(&p));
    Push clip{};
    clip.kind = PushKind_Clip;
    clip.clip = prIRectIntersect(r.clip, shapeBox(quad, prAffine2Identity()));
    coverageOf(r, quad, PrPathFillRule_NonZero, clip.clip, identity, &clip.coverage);
    clip.beneath = readsBeneath(commands + at, 1);
    Target* layer = newTarget(clip.clip, BL_FORMAT_PRGB32, tangent);
    if (clip.beneath && !prIRectIsEmpty(clip.clip))
        copyBeneath(r, layer);
    r.targets.push(layer);
    r.pushes.push(clip);
    Push group{};
    group.kind = PushKind_Group;
    group.clip = clip.clip;
    group.group = g;
    group.composite = c.composite;
    group.blendMode = c.blendMode;
    r.targets.push(newTarget(group.clip, BL_FORMAT_PRGB32, tangent));
    r.pushes.push(group);
    r.clip = group.clip;
}

void pushGroup(Renderer& r, const PrDrawCommand& c) {
    const PrDrawGroup* g = c.group;
    Push push{};
    push.kind = PushKind_Plain;
    push.clip = r.clip;
    if (r.collapsed != 0 || !prDrawGroupHasEffect(g)) {
        r.pushes.push(push);
        return;
    }
    push.kind = PushKind_Group;
    push.group = g;
    push.composite = c.composite;
    push.blendMode = c.blendMode;
    PrIRect box = r.clip;
    if (!prRectIsEmpty(g->bounds) && !prDrawCompositeIsUnbounded(g->composite))
        box = prIRectIntersect(box, deviceBox(toTarget(r), g->bounds));
    push.clip = box;
    r.targets.push(newTarget(box, BL_FORMAT_PRGB32, userOf(r)));
    r.pushes.push(push);
    r.clip = box;
}

void pop(Renderer& r) {
    Push push = r.pushes.data[--r.pushes.count];
    r.clip = r.pushes.count != 0 ? r.pushes.last().clip : r.targets.data[0]->box;
    if (push.collapsed)
        --r.collapsed;
    switch (push.kind) {
        case PushKind_Transform:
        case PushKind_Rect:
            bl_context_restore(context(r), nullptr);
            break;
        case PushKind_Clip: {
            Target* layer = r.targets.data[--r.targets.count];
            endTarget(layer);
            readyForOffscreen(&target(r));
            // Blends the offscreen back through the clip's coverage. It is copied if it started as what lies beneath,
            // and composited source-over if it started transparent.
            BLContextCore* ctx = context(r);
            beginDevice(ctx);
            BLPatternCore pattern{};
            targetPattern(layer, &pattern);
            bl_context_set_fill_style(ctx, &pattern);
            bl_pattern_destroy(&pattern);
            bl_context_set_comp_op(ctx, push.beneath ? BL_COMP_OP_SRC_COPY : BL_COMP_OP_SRC_OVER);
            BLPoint origin{double(layer->box.left), double(layer->box.top)};
            bl_context_fill_mask_d(ctx, &origin, &push.coverage, nullptr);
            endDevice(ctx);
            bl_image_destroy(&push.coverage);
            destroyTarget(layer);
            break;
        }
        case PushKind_Group: {
            Target* layer = r.targets.data[--r.targets.count];
            endTarget(layer);
            readyForOffscreen(&target(r));
            BLImageCore mask{};
            if (push.group->mask != nullptr)
                maskOf(r, push, layer->box, &mask);
            BLContextCore* ctx = context(r);
            beginDevice(ctx);
            bl_context_set_comp_op(ctx, compOpOf(push.composite));
            bl_context_set_global_alpha(ctx, double(prF32Clamp(push.group->opacity, 0, 1)));
            BLPoint origin{double(layer->box.left), double(layer->box.top)};
            if (push.group->mask != nullptr) {
                BLPatternCore pattern{};
                targetPattern(layer, &pattern);
                bl_context_set_fill_style(ctx, &pattern);
                bl_pattern_destroy(&pattern);
                bl_context_fill_mask_d(ctx, &origin, &mask, nullptr);
                bl_image_destroy(&mask);
            } else {
                bl_context_blit_image_d(ctx, &origin, &layer->image, nullptr);
            }
            endDevice(ctx);
            destroyTarget(layer);
            // A projected group's offscreen composites into the clip to where its plane lands, which pops with it.
            if (push.group->projection != nullptr)
                pop(r);
            break;
        }
        default:
            break;
    }
}

void clearTo(Renderer& r, PrColor color) {
    BLContextCore* ctx = context(r);
    BLRgba rgba = rgbaOf(color);
    bl_context_set_comp_op(ctx, BL_COMP_OP_SRC_COPY);
    bl_context_set_fill_style_rgba(ctx, &rgba);
    bl_context_fill_all(ctx);
    bl_context_set_comp_op(ctx, BL_COMP_OP_SRC_OVER);
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

void initRenderer(Renderer& r, Blend2dBackend* b2, Target* root, const PrPixels* PR_NULLABLE pixels) {
    r.b2 = b2;
    r.pixels = pixels;
    r.targets.push(root);
    r.clip = root->box;
    r.shape = prPathCreate();
    r.extra = prPathCreate();
    bl_path_init(&r.path);
}

void finishRenderer(Renderer& r) {
    r.pushes.reset();
    r.targets.reset();
    r.rampPieces.reset();
    r.ramp.reset();
    r.stops.reset();
    prPathDestroy(r.shape);
    prPathDestroy(r.extra);
    PrPathPolyline* polylines[] = {&r.outline, &r.combined};
    for (PrPathPolyline* polyline : polylines)
        prPathPolylineReset(polyline);
    bl_path_destroy(&r.path);
}

void runCommands(Renderer& r, const PrDrawCommand* commands, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = commands[i];
        bool skipped = r.collapsed != 0 && prDrawCommandSkipsCollapsed(&c);
        if (skipped && c.kind != PrDrawCommandKind_PushGroup && !(c.kind >= PrDrawCommandKind_PushClipRect &&
                c.kind <= PrDrawCommandKind_PushClipPath))
            continue;
        switch (c.kind) {
            case PrDrawCommandKind_Clear:
                clearTo(r, c.color);
                break;
            case PrDrawCommandKind_Border:
                drawBorder(r, c);
                break;
            case PrDrawCommandKind_DropShadow:
            case PrDrawCommandKind_InnerShadow:
                // Blend2D has no blur.
                break;
            case PrDrawCommandKind_Decoration:
                drawDecoration(r, c);
                break;
            case PrDrawCommandKind_NineSlice:
                drawNineSlice(r, c);
                break;
            case PrDrawCommandKind_WritePixels:
                writePixels(r, c);
                break;
            case PrDrawCommandKind_Glyphs:
                drawGlyphs(r, c);
                break;
            case PrDrawCommandKind_Embed: {
                // The offset plays as a push of its translation.
                PrDrawCommand moves[2]{};
                moves[0].kind = PrDrawCommandKind_PushTransformAffine2;
                moves[0].affine = prDAffine2Translate(c.embed.offset.x, c.embed.offset.y);
                moves[1].kind = PrDrawCommandKind_PopTransform;
                runCommands(r, &moves[0], 1);
                runCommands(r, prDrawListCommands(c.embed.list), prDrawListCommandCount(c.embed.list));
                runCommands(r, &moves[1], 1);
                break;
            }
            case PrDrawCommandKind_PushTransformAffine2: {
                // Nothing draws under a collapsed transform, so Blend2D is not given it.
                PrDAffine2 pushed = c.affine;
                Push push{};
                push.kind = PushKind_Transform;
                push.clip = r.clip;
                push.collapsed = r.collapsed != 0 ||
                    prDrawAffine2Collapses(prAffine2Concat(prAffine2FromDAffine2(pushed), toTarget(r)));
                bl_context_save(context(r), nullptr);
                if (push.collapsed) {
                    ++r.collapsed;
                } else {
                    BLMatrix2D m = matrixOf(pushed);
                    bl_context_apply_transform_op(context(r), BL_TRANSFORM_OP_TRANSFORM, &m);
                }
                r.pushes.push(push);
                break;
            }
            case PrDrawCommandKind_PopTransform:
                pop(r);
                break;
            case PrDrawCommandKind_PushClipRect:
            case PrDrawCommandKind_PushClipTargetRect:
            case PrDrawCommandKind_PushClipCornerRect:
            case PrDrawCommandKind_PushClipPath:
                pushClip(r, commands, count, i);
                break;
            case PrDrawCommandKind_PushGroup:
                if (c.group->projection != nullptr)
                    pushProjectedGroup(r, commands, i);
                else
                    pushGroup(r, c);
                break;
            case PrDrawCommandKind_PopClip:
            case PrDrawCommandKind_PopGroup:
                pop(r);
                break;
            case PrDrawCommandKind_PushMark:
            case PrDrawCommandKind_PopMark:
                break;
            default:
                drawShape(r, c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The backend

void destroyBackend(PrDrawRenderer* renderer) {
    Blend2dBackend* b2 = static_cast<Blend2dBackend*>(renderer);
    for (uint32_t i = 0; i < b2->images.count; ++i)
        bl_image_destroy(&b2->images.data[i].image);
    b2->images.reset();
    b2->imageIndex.reset();
    prDrawGlyphCacheDestroy(b2->glyphs);
    if (b2->ownsDevice)
        prGpuDeviceDestroy(b2->device);
    prMemoryFree(b2);
}

void destroyFrame(PrDrawRenderer*, PrDrawFrame* frame) {
    drawMemoryFrameDestroy(static_cast<DrawMemoryFrame*>(frame));
}

// Drops the cached data that the render did not use.
void endRender(Blend2dBackend* b2) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < b2->images.count; ++i) {
        if (b2->images.data[i].lastUsed == b2->render)
            b2->images.data[kept++] = b2->images.data[i];
        else
            bl_image_destroy(&b2->images.data[i].image);
    }
    b2->images.count = kept;
    b2->imageIndex.rebuild(kept, [&](uint32_t i) { return b2->images.data[i].id; });
    prDrawGlyphCacheEnd(b2->glyphs);
}

// Swaps the red and blue bytes of every pixel.
void swapRedBlue(PrPixels pixels) {
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        uint8_t* row = pixels.data + size_t(y) * pixels.stride;
        for (uint32_t x = 0; x < pixels.extent.x; ++x) {
            uint8_t swap = row[x * 4];
            row[x * 4] = row[x * 4 + 2];
            row[x * 4 + 2] = swap;
        }
    }
}

void renderPixels(PrDrawRenderer* renderer, PrPixels pixels, PrPixelFormat format, const PrDrawList* list,
        const PrDrawPart& part) {
    Blend2dBackend* b2 = static_cast<Blend2dBackend*>(renderer);
    // Blend2D draws premultiplied BGRA, so RGBA pixels are swapped for the render and swapped back afterward.
    bool swapped = format == PrPixelFormat_Rgba8;
    if (swapped)
        swapRedBlue(pixels);
    ++b2->render;
    prDrawGlyphCacheBegin(b2->glyphs);
    Target* root = allocZeroed<Target>();
    root->box = prIRectAt(PrIVec2{0, 0}, pixels.extent);
    bl_image_init(&root->image);
    bl_image_create_from_data(&root->image, int(pixels.extent.x), int(pixels.extent.y),
        BL_FORMAT_PRGB32, pixels.data, intptr_t(pixels.stride), BL_DATA_ACCESS_RW, nullptr, nullptr);
    beginTarget(root, b2->options.threads);
    Renderer r{};
    initRenderer(r, b2, root, &pixels);
    // The part's transform is the first transform of Blend2D's user space, which nothing pops.
    if (part.transform.degenerate) {
        ++r.collapsed;
    } else {
        BLMatrix2D m = matrixOf(part.transform.daffine);
        bl_context_apply_transform_op(context(r), BL_TRANSFORM_OP_TRANSFORM, &m);
    }
    runCommands(r, prDrawListCommands(list) + part.first, part.count);
    finishRenderer(r);
    endTarget(root);
    destroyTarget(root);
    if (swapped)
        swapRedBlue(pixels);
    endRender(b2);
}

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    return drawMemoryFrameCreate(renderer, renderPixels, false);
}

// Keeps the part of the list, which renders when the frame's pass runs.
PrStatus prepare(PrDrawRenderer*, PrDrawFrame* frame, const PrDrawList* list, const PrDrawPart* part,
        const PrDrawTarget*) {
    drawMemoryFramePrepare(static_cast<DrawMemoryFrame*>(frame), list, *part);
    return PrStatus_Ok;
}

void renderFrame(PrDrawRenderer*, PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    drawMemoryFrameRender(static_cast<DrawMemoryFrame*>(frame), pass, origin);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawBlend2dOptions prDrawBlend2dOptionsDefault(void) {
    return PrDrawBlend2dOptions{};
}

PrDrawRenderer* PR_NULLABLE prDrawBlend2dCreate(const PrDrawBlend2dOptions* PR_NULLABLE options) {
    PrDrawBlend2dOptions o = options != nullptr ? *options : prDrawBlend2dOptionsDefault();
    bool owned = false;
    PrGpuDevice* device = drawMemoryDevice(o.device, &owned);
    if (device == nullptr)
        return nullptr;
    Blend2dBackend* b2 = allocZeroed<Blend2dBackend>();
    b2->device = device;
    b2->ownsDevice = owned;
    b2->options = o;
    b2->glyphs = prDrawGlyphCacheCreate(b2->options.glyphDecoder);
    b2->destroy = destroyBackend;
    b2->frameCreate = createFrame;
    b2->frameDestroy = destroyFrame;
    b2->framePrepare = prepare;
    b2->frameRender = renderFrame;
    return b2;
}
