// The CPU draw backend, which renders into pixels in memory.
//
// Every painting command becomes coverage and a paint. A shape's coverage comes from the path rasterizer, and a
// stroke's coverage comes from its lowered outline. The paint composites through the coverage into the target. A clip
// is a coverage mask made at its push. A group draws into an offscreen, where its contents see only the clips pushed
// inside the group. At its pop, the group composites through the clip that was in effect at its push. A projected
// group's offscreen holds its plane, and composites through the projection.

#include "pr/draw_cpu.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_projection.h"
#include "pr/draw_transform.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/path_rasterizer.h"
#include "pr/vec.h"

#include "draw/backends/cpu/src/cpu_filter.h"
#include "draw/backends/cpu/src/cpu_pixels.h"
#include "draw/backends/shared/src/draw_shared_layer.h"

namespace pr_impl {

namespace {

// The maximum distance of a flattened curve, or of a stroke's round parts, from the true shape, in target pixels.
constexpr float kTolerance = 0.05f;

// The colors per unit of a gradient's parameter in a ramp's pieces that mix nonlinearly.
constexpr uint32_t kRampResolution = 1024;

// The largest side length of a texture that a list renders into.
constexpr uint32_t kListTextureMaxSize = 8192;

// A clip in effect: the target pixels that it can let through, their coverage by the clips that are not rectangles, and
// the intersection of the rectangle clips in force. A pixel's coverage is the mask's coverage, or full coverage without
// a mask, times the area of the pixel inside the rectangle. That area is whole within the box unless `edges` is set.
struct Clip {
    CpuCoverage coverage;
    PrRect rect;                // in target pixels
    bool edges;                 // true if the rectangle's edges cut pixels of the box
};

// Returns a clip that lets the whole box through.
Clip clipOfBox(PrIRect box) {
    return Clip{CpuCoverage{nullptr, 0, box}, prRectFromIRect(box), false};
}

// Returns the clip's coverage of `count` pixels of row `y` from `x`, or null if it lets them all through. `scratch`
// holds kShadeSpan values.
const uint8_t* PR_NULLABLE clipSpan(const Clip& clip, int32_t x, int32_t y, uint32_t count, uint8_t* scratch) {
    const uint8_t* span = cpuCoverageSpan(clip.coverage, x, y, count, scratch);
    if (!clip.edges)
        return span;
    const PrRect& r = clip.rect;
    float row = prF32Clamp(prF32Min(float(y) + 1, r.bottom) - prF32Max(float(y), r.top), 0, 1);
    if (row >= 1 && float(x) >= r.left && float(x) + float(count) <= r.right)
        return span;
    if (span == nullptr)
        prMemFill(scratch, 255, count);
    else if (span != scratch)
        prMemCopy(scratch, span, count);
    for (uint32_t i = 0; i < count; ++i) {
        float px = float(x) + float(i);
        float column = prF32Clamp(prF32Min(px + 1, r.right) - prF32Max(px, r.left), 0, 1);
        scratch[i] = uint8_t(float(scratch[i]) * column * row + 0.5f);
    }
    return scratch;
}

// Scales the target's pixels by the clip's coverage.
void maskByClip(const CpuTarget& t, const Clip& clip) {
    uint8_t scratch[kShadeSpan];
    uint32_t width = uint32_t(prIRectWidth(t.box));
    for (int32_t y = t.box.top; y < t.box.bottom; ++y) {
        uint8_t* row = t.data + size_t(y - t.box.top) * t.stride;
        for (uint32_t start = 0; start < width; start += kShadeSpan) {
            uint32_t count = width - start < kShadeSpan ? width - start : kShadeSpan;
            const uint8_t* c = clipSpan(clip, t.box.left + int32_t(start), y, count, scratch);
            if (c == nullptr)
                continue;
            for (uint32_t i = 0; i < count; ++i)
                cpuScalePixel(row + size_t(start + i) * cpuPixelBytes(t.format), t.format, c[i]);
        }
    }
}

// A group push: whether it drew into an offscreen, and what its pop composites and where it composites it.
struct Frame {
    bool layered;
    const PrDrawGroup* group;
    PrDrawComposite composite;
    PrDrawBlendMode blendMode;
    PrDrawTransform xform;          // the group's space to its offscreen's pixels
    bool local;                     // the offscreen is in the group's space, with equal scale on both axes
    PrDrawTransform localToDevice;  // local: the offscreen's pixels to the target's
    bool projected;                 // the offscreen holds the group's plane (PrDrawGroup::projection)
    PrDrawProjection projection;    // projected: where the offscreen lands
    uint32_t clipDepth;             // the number of clips in force at the push
    CpuTarget parent;

    // local: what lies beneath, resampled into the offscreen's space. projected: what lies beneath the plane, resampled
    // into the offscreen through the projection once a backdrop reads it (`underReady`). A projected group is not a
    // backdrop root.
    CpuTarget under;
    bool underReady;

    // A group with a layer of the host's (PrDrawGroup::layer) is local, and its offscreen is the host's pixels. `drawn`
    // is the revision that they held when the render claimed the group's contents (drawSharedClaimLayer).
    bool host;
    uint64_t drawn;

    bool hidden;                    // its contents draw nothing that shows, under a clip of nothing
};

// A gradient's ramp, created once per render for each brush that needs one. Its storage is kept for later renders.
struct Ramp {
    const PrDrawBrush* brush;
    Array<PrDrawRampPiece> pieces;
    Array<PrColor> colors;          // premultiplied, without a color
};

struct CpuBackend;

// The state of a render as it walks the commands. The backend keeps one for each depth of nesting, because a list
// renders into a texture with the next depth. A render therefore reuses the storage that the last render at its depth
// grew: its stacks, scratch paths and masks, and the pixels of clip masks and offscreens, indexed by push depth.
struct Renderer {
    CpuBackend* cpu;
    uint32_t depth;
    bool used;                      // true if the render in progress uses it
    CpuTarget target;
    Array<PrDrawTransform> transforms;
    Array<Clip> clips;
    Array<Frame> frames;
    Array<Array<uint8_t>> clipMasks;    // for each clip in `clips`
    Array<Array<uint8_t>> layers;       // groups' offscreens, by the group's index in `frames`
    Array<Array<uint8_t>> unders;       // Frame::under's pixels, likewise
    Array<Array<uint8_t>> seen;         // a projected group's offscreen over what lies beneath it, likewise
    Array<uint8_t> copy;                // a target's pixels, copied as a texture's texels
    PrPathRasterizer* rasterizer;
    PrPath* shape;
    PrPath* extra;
    PrPathPolyline landed;                  // a shape in target pixels
    PrPathPolyline cut;                     // another shape, which cuts the first
    PrPathPolyline fill;                    // a stroke's outlines (prDrawLowerStroke)
    PrPathPolyline aligned;
    PrPathPolyline region;
    Array<uint8_t> coverage;
    Array<uint8_t> second;
    Array<Ramp> ramps;                  // the first `rampCount` are this render's
    uint32_t rampCount;
    HashIndex rampIndex;                // this render's ramps, by the inputs that decide their colors
    Array<PrDrawImageRect> pieces;
};

// The backend, and the state that it keeps between renders: textures of images and lists, the lists that color glyphs
// paint, and a renderer for each depth of nesting. It drops whatever a render does not use.
struct CpuBackend : PrDrawRenderer {
    bool glyphPhases;                   // PrDrawCpuOptions
    CpuTextureCache textures;
    PrDrawGlyphCache* glyphs;
    Array<Renderer*> renderers;         // for each depth of nesting
};

// A frame: the list that it renders once there are pixels.
struct CpuFrame : PrDrawFrame {
    const PrDrawList* list;
};

// Flattens the path into `out` in target pixels under the transform, and returns `out`.
const PrPathPolyline& land(const PrDrawTransform& x, const PrPath* path, PrPathPolyline& out) {
    prPathPolylineClear(&out);
    prPathFlatten(path, x.affine, kTolerance, PrPathFlattening_Balanced, &out);
    return out;
}

const Clip& currentClip(const Renderer& r) {
    return r.clips.last();
}

// Returns room for `size` bytes in the storage at `index` of `pool`.
uint8_t* pooled(Array<Array<uint8_t>>& pool, uint32_t index, size_t size) {
    while (pool.count <= index)
        pool.push(Array<uint8_t>{});
    Array<uint8_t>& storage = pool.data[index];
    if (storage.capacity < size) {
        storage.reset();
        storage.reserve(uint32_t(size != 0 ? size : 1));
    }
    return storage.data;
}

void resetPool(Array<Array<uint8_t>>& pool) {
    for (uint32_t i = 0; i < pool.count; ++i)
        pool.data[i].reset();
    pool.reset();
}

// Returns the backend's renderer at a depth of nesting, prepared to render onto the target.
Renderer& beginRender(CpuBackend* cpu, uint32_t depth, const CpuTarget& target) {
    while (cpu->renderers.count <= depth) {
        Renderer* r = allocZeroed<Renderer>();
        r->cpu = cpu;
        r->depth = cpu->renderers.count;
        r->rasterizer = prPathRasterizerCreate();
        r->shape = prPathCreate();
        r->extra = prPathCreate();
        cpu->renderers.push(r);
    }
    Renderer& r = *cpu->renderers.data[depth];
    PR_ASSERT(r.transforms.count == 0);
    r.used = true;
    r.target = target;
    r.rampCount = 0;
    r.rampIndex.clear();
    r.transforms.push(prDrawTransformFromAffine2(prAffine2Identity()));
    r.clips.push(clipOfBox(target.box));
    return r;
}

void endRender(Renderer& r) {
    r.transforms.clear();
    r.clips.clear();
    r.frames.clear();
}

void destroyRenderer(Renderer* r) {
    r->transforms.reset();
    r->clips.reset();
    r->frames.reset();
    resetPool(r->clipMasks);
    resetPool(r->layers);
    resetPool(r->unders);
    resetPool(r->seen);
    r->copy.reset();
    prPathRasterizerDestroy(r->rasterizer);
    prPathDestroy(r->shape);
    prPathDestroy(r->extra);
    prPathPolylineReset(&r->landed);
    prPathPolylineReset(&r->cut);
    prPathPolylineReset(&r->fill);
    prPathPolylineReset(&r->aligned);
    prPathPolylineReset(&r->region);
    r->coverage.reset();
    r->second.reset();
    for (uint32_t i = 0; i < r->ramps.count; ++i) {
        r->ramps.data[i].pieces.reset();
        r->ramps.data[i].colors.reset();
    }
    r->ramps.reset();
    r->rampIndex.reset();
    r->pieces.reset();
    prMemoryFree(r);
}

// ---------------------------------------------------------------------------------------------------------------------
// Coverage

// Returns how a command's coverage is computed: by area if it is antialiased, and by pixel centers otherwise.
PrPathCoverage coverageOf(bool antialias) {
    return antialias ? PrPathCoverage_Area : PrPathCoverage_Center;
}

// Returns room in `storage` for a mask over `box`.
CpuCoverage maskOver(Array<uint8_t>& storage, PrIRect box) {
    size_t size = size_t(prIRectWidth(box)) * prIRectHeight(box);
    if (storage.capacity < size) {
        storage.reset();
        storage.reserve(uint32_t(size));
    }
    return CpuCoverage{storage.data, size_t(prIRectWidth(box)), box};
}

// Rasterizes the polyline over exactly `box`.
void rasterizeOver(Renderer& r, const PrPathPolyline& polyline, PrPathFillRule rule, PrPathCoverage coverage,
        const CpuCoverage& mask) {
    if (prIRectIsEmpty(mask.box))
        return;
    prPathRasterizerFill(r.rasterizer, &polyline, rule, coverage, PrIVec2{mask.box.left, mask.box.top},
        PrPixels{mask.data, mask.stride, {prIRectWidth(mask.box), prIRectHeight(mask.box)}});
}

// Returns the box that the polyline covers, within `limit`.
PrIRect polylineBox(const PrPathPolyline& polyline, PrIRect limit) {
    if (polyline.pointCount == 0)
        return PrIRect{0, 0, 0, 0};
    return prIRectIntersect(prIRectRoundOut(prPathPolylineBounds(&polyline)), limit);
}

// Computes the coverage of `r.shape`, filled or stroked as the command specifies, over the area that it covers within
// `limit`. Returns false if it covers nothing.
bool pathCoverage(Renderer& r, const PrDrawCommand& c, PrIRect limit, CpuCoverage* out) {
    const PrDrawTransform& x = r.transforms.last();
    if (c.stroke == nullptr) {
        const PrPathPolyline& landed = land(x, r.shape, r.landed);
        PrIRect box = polylineBox(landed, limit);
        if (prIRectIsEmpty(box))
            return false;
        *out = maskOver(r.coverage, box);
        rasterizeOver(r, landed, c.rule, coverageOf(c.antialias), *out);
        return true;
    }
    prPathPolylineClear(&r.fill);
    prPathPolylineClear(&r.aligned);
    prPathPolylineClear(&r.region);
    prDrawLowerStroke(r.shape, c.stroke, x.affine, kTolerance, &r.fill, &r.aligned, &r.region);
    PrIRect box = prIRectUnion(polylineBox(r.fill, limit), polylineBox(r.aligned, limit));
    if (prIRectIsEmpty(box))
        return false;
    *out = maskOver(r.coverage, box);
    if (r.aligned.pointCount == 0) {
        rasterizeOver(r, r.fill, PrPathFillRule_NonZero, coverageOf(c.antialias), *out);
        return true;
    }
    // Cuts the aligned outline to the region's interior or exterior, and unites it with the outline of the open
    // contours.
    rasterizeOver(r, r.aligned, PrPathFillRule_NonZero, coverageOf(c.antialias), *out);
    CpuCoverage region = maskOver(r.second, box);
    rasterizeOver(r, r.region, PrPathFillRule_NonZero, coverageOf(c.antialias), region);
    bool inside = c.stroke->align == PrDrawStrokeAlign_Inside;
    size_t size = size_t(prIRectWidth(box)) * prIRectHeight(box);
    for (size_t i = 0; i < size; ++i) {
        uint32_t cut = inside ? region.data[i] : 255u - region.data[i];
        out->data[i] = uint8_t(cpuDivide255(out->data[i] * cut));
    }
    if (r.fill.pointCount != 0) {
        rasterizeOver(r, r.fill, PrPathFillRule_NonZero, coverageOf(c.antialias), region);
        for (size_t i = 0; i < size; ++i)
            out->data[i] = out->data[i] > region.data[i] ? out->data[i] : region.data[i];
    }
    return true;
}

// Computes the coverage of a shape command, filled or stroked, over the area that it covers within `limit`. Returns
// false if it covers nothing.
bool shapeCoverage(Renderer& r, const PrDrawCommand& c, PrIRect limit, CpuCoverage* out) {
    if (c.kind == PrDrawCommandKind_Line && c.stroke == nullptr)
        return false;
    prPathClear(r.shape);
    prDrawShapePath(&c, r.shape);
    return pathCoverage(r, c, limit, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// Returns the texture that an area of a list renders to, for the list brush to sample as an image. Its size is that of
// `tile` at `scale` target pixels per unit of brush space along each axis.
CpuTexture* listTexture(Renderer& r, const PrDrawList* drawn, PrRect area, PrRect tile, PrVec2 scale);

// Returns the gradient's ramp, which is created on first use in a render by any gradient with the same ramp.
const Ramp& rampOf(Renderer& r, const PrDrawBrush* brush) {
    constexpr PrColor kWhite{1, 1, 1, 1};
    const uint32_t hash = uint32_t(prDrawGradientRampHash(brush, kWhite));
    const uint32_t found = r.rampIndex.find(hash, [&](uint32_t i) {
        return prDrawGradientRampEquals(r.ramps.data[i].brush, kWhite, brush, kWhite);
    });
    if (found != kHashMissing)
        return r.ramps.data[found];
    r.rampIndex.insert(hash, r.rampCount);
    if (r.rampCount == r.ramps.count)
        r.ramps.push(Ramp{});
    Ramp& ramp = r.ramps.data[r.rampCount++];
    ramp.brush = brush;
    ramp.pieces.resize(prDrawGradientRampMaxPieces(brush->stopCount));
    ramp.colors.resize(prDrawGradientRampMaxColors(brush->stopCount, kRampResolution));
    ramp.pieces.resize(prDrawGradientRamp(brush, kWhite, PrColorSpace_Srgb, kRampResolution, ramp.pieces.data,
        ramp.colors.data));
    ramp.colors.resize(prDrawRampColorCount(ramp.pieces.data, ramp.pieces.count));
    return ramp;
}

// Places a stage whose space `fromUser` maps user space into. Target pixels map to user space through the inverse of
// the current transform. Returns false if the transform cannot be inverted.
bool placeFromUser(const PrDrawTransform& x, PrAffine2 fromUser, CpuPlacement* out) {
    *out = CpuPlacement{};
    PrAffine2 userFromDevice{};
    if (!prAffine2Invert(x.affine, &userFromDevice))
        return false;
    out->transform = prAffine2Concat(userFromDevice, fromUser);
    return true;
}

// Adds an image command's texels as the paint's next stage, clamped at the edges of `texels`.
void addImageStage(CpuPaint* paint, const CpuTexture* texture, PrRect texels, PrDrawSampling sampling,
        const CpuPlacement& placement) {
    CpuStage& stage = paint->stages[paint->stageCount++];
    stage.placement = placement;
    stage.source.kind = SourceKind_Texture;
    stage.source.texture = texture;
    stage.source.texels = texels;
    stage.source.tiles = PrDrawTiles{.tile = texels, .x = PrDrawExtend_Pad, .y = PrDrawExtend_Pad};
    stage.source.sampling = PrDrawSampling(sampling & PrDrawSamplingFlag_Kind);
}

// Makes the paint of a painting command, with its brush as its first stage. Returns false if it paints nothing.
// `bounds` is the command's fill geometry bounds, for an ObjectBounds brush. For a stroke whose brush is mapped along
// it, `unitPixels` is the number of target pixels that one unit of its unit square covers along each axis.
bool makePaint(Renderer& r, const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds,
        const PrDrawTransform& x, CpuPaint* paint, const PrVec2* PR_NULLABLE unitPixels = nullptr) {
    *paint = CpuPaint{};
    float alpha = prF32Clamp(color.a, 0, 1);
    paint->color[0] = color.r * alpha;
    paint->color[1] = color.g * alpha;
    paint->color[2] = color.b * alpha;
    paint->color[3] = alpha;
    if (brush == nullptr)
        return alpha > 0;
    CpuStage& stage = paint->stages[paint->stageCount++];
    CpuPlacement& placement = stage.placement;
    if (!prDrawBrushFromTarget(brush, x.affine, bounds, &placement.transform))
        return false;
    CpuSource& source = stage.source;
    if (brush->kind != PrDrawBrushKind_Image) {
        source.kind = SourceKind_Gradient;
        source.brush = brush;
        const Ramp& ramp = rampOf(r, brush);
        source.rampPieces = ramp.pieces.data;
        source.ramp = ramp.colors.data;
        return true;
    }
    source.kind = SourceKind_Texture;
    source.tiles = brush->image.tiles;
    source.limit = brush->image.limit;
    source.sampling = PrDrawSampling(brush->image.sampling & PrDrawSamplingFlag_Kind);
    const PrDrawSource& image = brush->image.source;
    if (prRectIsEmpty(source.tiles.tile) || !prDrawSourceIsValid(image))
        return false;
    CpuTexture* texture = nullptr;
    if (image.kind == PrDrawSourceKind_Image) {
        texture = cpuTextureFromImage(r.cpu->textures, image.image);
        source.texels = image.rect;
    } else if (image.kind != PrDrawSourceKind_List) {
        return false;
    } else {
        PrAffine2 toDevice{};
        if (!prAffine2Invert(placement.transform, &toDevice))
            return false;
        PrVec2 scale{};
        if (unitPixels != nullptr) {
            // The brush's transform maps brush space into the unit square, which spans the stroke's length and width.
            const PrAffine2& t = brush->transform;
            float ax = t.a * unitPixels->x, by = t.b * unitPixels->y;
            float cx = t.c * unitPixels->x, dy = t.d * unitPixels->y;
            scale.x = prF32Sqrt(ax * ax + by * by);
            scale.y = prF32Sqrt(cx * cx + dy * dy);
        } else {
            float uniform = prAffine2MaxScale(toDevice);
            scale = PrVec2{uniform, uniform};
        }
        texture = listTexture(r, image.list, image.rect, source.tiles.tile, scale);
        if (texture == nullptr)
            return false;
        source.texels = PrRect{0, 0, float(texture->levels.data[0].width), float(texture->levels.data[0].height)};
    }
    if (prRectIsEmpty(source.texels))
        return false;
    cpuPrepareMips(*texture, source.sampling);
    source.texture = texture;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Compositing

// Finds the part [*first, *end) of `count` pixels where both coverages are nonzero. The part is empty if there is none.
void trimSpan(const uint8_t* PR_NULLABLE shape, const uint8_t* PR_NULLABLE clip, uint32_t count, uint32_t* first,
        uint32_t* end) {
    auto passes = [&](uint32_t i) {
        return (shape == nullptr || shape[i] != 0) && (clip == nullptr || clip[i] != 0);
    };
    *first = 0;
    *end = count;
    while (*first < *end && !passes(*first))
        ++*first;
    while (*end > *first && !passes(*end - 1))
        --*end;
}

// Returns the box that a draw visits, within the clip and the target. This is the shape's box, or the clip's box for a
// layer whose operator changes what lies outside it.
PrIRect visitBox(const Renderer& r, PrIRect shape, PrDrawComposite op, bool layer) {
    const PrIRect& clip = currentClip(r).coverage.box;
    bool whole = layer && prDrawCompositeIsUnbounded(op);
    return prIRectIntersect(whole ? clip : shape, prIRectIntersect(clip, r.target.box));
}

// Composites the paint into the target where the shape covers it and the clip lets it through (cpuCompositeSpan). A
// layer's paint is its pixels, which a group composites.
void composite(Renderer& r, const CpuCoverage& shape, const CpuPaint& paint, PrDrawComposite op,
        PrDrawBlendMode blend, bool layer = false) {
    PrIRect box = visitBox(r, shape.box, op, layer);
    if (prIRectIsEmpty(box))
        return;
    r.target.drawn = prIRectUnion(r.target.drawn, box);
    const CpuTarget& t = r.target;
    const Clip& clip = currentClip(r);
    bool bounded = !(layer && prDrawCompositeIsUnbounded(op));
    uint32_t width = uint32_t(prIRectWidth(box));
    uint8_t shapeScratch[kShadeSpan];
    uint8_t clipScratch[kShadeSpan];
    float colors[kShadeSpan * 4];
    for (int32_t y = box.top; y < box.bottom; ++y) {
        uint8_t* row = cpuTargetRow(t, y);
        for (uint32_t start = 0; start < width; start += kShadeSpan) {
            uint32_t count = width - start < kShadeSpan ? width - start : kShadeSpan;
            int32_t x0 = box.left + int32_t(start);
            const uint8_t* shapeRow = cpuCoverageSpan(shape, x0, y, count, shapeScratch);
            const uint8_t* clipRow = clipSpan(clip, x0, y, count, clipScratch);
            uint32_t first = 0;
            uint32_t end = count;
            // Trimming to nonzero coverage is only worthwhile for shading.
            if (bounded && paint.stageCount != 0) {
                trimSpan(shapeRow, clipRow, count, &first, &end);
                if (first == end)
                    continue;
            }
            int32_t x = x0 + int32_t(first);
            const float* source = paint.color;
            uint32_t stride = 0;
            if (paint.stageCount != 0) {
                cpuShade(paint, x, y, end - first, colors);
                source = colors;
                stride = 4;
            }
            cpuCompositeSpan(row + size_t(x - t.box.left) * cpuPixelBytes(t.format), t.format, source, stride,
                shapeRow != nullptr ? shapeRow + first : nullptr,
                clipRow != nullptr ? clipRow + first : nullptr, end - first, op, blend, t.knockout, layer);
        }
    }
}

// Composites an offscreen of the target's format onto the target, where the offscreen has drawn and the clip lets it
// through.
void compositeLayer(Renderer& r, const CpuTarget& layer, PrDrawComposite op, PrDrawBlendMode blend) {
    PrIRect box = visitBox(r, layer.drawn, op, true);
    if (prIRectIsEmpty(box))
        return;
    r.target.drawn = prIRectUnion(r.target.drawn, box);
    const CpuTarget& t = r.target;
    const Clip& clip = currentClip(r);
    CpuCoverage shape{nullptr, 0, layer.drawn};
    uint32_t width = uint32_t(prIRectWidth(box));
    uint8_t shapeScratch[kShadeSpan];
    uint8_t clipScratch[kShadeSpan];
    uint8_t pixels[kShadeSpan * 8];
    const uint32_t bytes = cpuPixelBytes(t.format);
    PR_ASSERT(layer.format == t.format);
    for (int32_t y = box.top; y < box.bottom; ++y) {
        uint8_t* row = cpuTargetRow(t, y);
        for (uint32_t start = 0; start < width; start += kShadeSpan) {
            uint32_t count = width - start < kShadeSpan ? width - start : kShadeSpan;
            int32_t x = box.left + int32_t(start);
            const uint8_t* shapeRow = cpuCoverageSpan(shape, x, y, count, shapeScratch);
            const uint8_t* clipRow = clipSpan(clip, x, y, count, clipScratch);
            // The offscreen's pixels, transparent outside its box.
            const uint8_t* source = pixels;
            PrIRect span{x, y, x + int32_t(count), y + 1};
            PrIRect inside = prIRectIntersect(span, layer.box);
            if (prIRectContains(layer.box, span)) {
                source = cpuTargetPixel(layer, x, y);
            } else {
                prMemZero(pixels, size_t(count) * bytes);
                if (!prIRectIsEmpty(inside))
                    prMemCopy(pixels + size_t(inside.left - x) * bytes, cpuTargetPixel(layer, inside.left, y),
                        size_t(prIRectWidth(inside)) * bytes);
            }
            cpuCompositePixels(row + size_t(x - t.box.left) * bytes, t.format, source, shapeRow,
                clipRow, count, op, blend, t.knockout);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands

void runCommands(Renderer& r, const PrDrawCommand* commands, uint32_t count);
void drawShapeWith(Renderer& r, const PrDrawCommand& c, const CpuPaint& paint);

void clearTarget(Renderer& r, PrColor color) {
    CpuPaint paint{};
    makePaint(r, nullptr, color, PrRect{}, r.transforms.last(), &paint);
    const Clip& clip = currentClip(r);
    PrIRect box = prIRectIntersect(clip.coverage.box, r.target.box);
    if (clip.coverage.data == nullptr && !clip.edges && !r.target.knockout) {
        r.target.drawn = prIRectUnion(r.target.drawn, box);
        const uint32_t bytes = cpuPixelBytes(r.target.format);
        uint8_t pixel[8];
        cpuWritePixel(pixel, r.target.format, paint.color);
        for (int32_t y = box.top; y < box.bottom; ++y) {
            for (int32_t x = box.left; x < box.right; ++x)
                prMemCopy(cpuTargetPixel(r.target, x, y), pixel, bytes);
        }
        return;
    }
    composite(r, CpuCoverage{nullptr, 0, box}, paint, PrDrawComposite_Copy, PrDrawBlendMode_Normal);
}

// A stroke whose brush is mapped along it. It holds the centerline measured in target pixels, in which the paint looks
// up each pixel, and the number of target pixels that one unit of the unit square covers along each axis.
struct StrokeMap {
    PrPathMeasure* PR_NULLABLE measure;
    PrAffine2 unitToBrush;
    float scale;
    float reach;
    PrVec2 unitPixels;
};

// Measures a mapped stroke. Returns false if its brush maps nowhere. The caller destroys the measure.
bool measureStroke(Renderer& r, const PrDrawCommand& c, StrokeMap* out) {
    *out = StrokeMap{};
    if (!prAffine2Invert(c.brush->transform, &out->unitToBrush))
        return false;
    const PrDrawTransform& x = r.transforms.last();
    prPathClear(r.shape);
    prDrawShapePath(&c, r.shape);
    prPathPolylineClear(&r.landed);
    prPathFlatten(r.shape, x.affine, kTolerance, PrPathFlattening_Inside, &r.landed);
    const PrDrawStroke& stroke = *c.stroke;
    float scale = stroke.nonScaling ? 1 : prAffine2MaxScale(x.affine);
    out->scale = scale;
    out->reach = prDrawStrokeReach(&stroke) * stroke.shape.width * scale;
    out->measure = prPathMeasureCreate();
    prPathMeasureSetPolyline(out->measure, &r.landed);
    float length = 0;
    for (uint32_t i = 0; i < prPathMeasureContourCount(out->measure); ++i)
        length = prF32Max(length, prPathMeasureContour(out->measure, i).length);
    float across = prF32Max(stroke.shape.width * scale, 1);
    out->unitPixels = stroke.mapping == PrDrawStrokeMapping_Along ? PrVec2{length, across} : PrVec2{across, length};
    return true;
}

// Places a stroke's brush in the unit square along it.
void mapStroke(const PrDrawCommand& c, const StrokeMap& map, CpuPaint* paint) {
    CpuPlacement& placement = paint->stages[0].placement;
    placement = CpuPlacement{};
    placement.kind = PlacementKind_Stroke;
    placement.transform = map.unitToBrush;
    placement.mapping = c.stroke->mapping;
    placement.measure = map.measure;
    placement.stroke = &c.stroke->shape;
    placement.strokeScale = map.scale;
    placement.reach = map.reach + 2;
}

bool mapsStroke(const PrDrawCommand& c) {
    return c.stroke != nullptr && c.brush != nullptr && c.stroke->mapping != PrDrawStrokeMapping_Plain;
}

void drawShape(Renderer& r, const PrDrawCommand& c) {
    StrokeMap map{};
    if (mapsStroke(c) && !measureStroke(r, c, &map))
        return;
    CpuPaint paint{};
    if (makePaint(r, c.brush, c.color, prDrawFillBounds(&c), r.transforms.last(), &paint,
            map.measure != nullptr ? &map.unitPixels : nullptr)) {
        if (map.measure != nullptr)
            mapStroke(c, map, &paint);
        drawShapeWith(r, c, paint);
    }
    prPathMeasureDestroy(map.measure);
}

void drawShapeWith(Renderer& r, const PrDrawCommand& c, const CpuPaint& paint) {
    CpuCoverage shape{};
    PrIRect limit = prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    if (shapeCoverage(r, c, limit, &shape))
        composite(r, shape, paint, c.composite, c.blendMode);
}

// Returns a texture of an image source if a unit of its rect covers `scale` target pixels along each axis, and writes
// the rect of the texels that the source spans. Returns null if the source has no texture.
CpuTexture* PR_NULLABLE sourceTexture(Renderer& r, const PrDrawSource& source, PrVec2 scale, PrRect* texels) {
    if (!prDrawSourceIsValid(source))
        return nullptr;
    if (source.kind == PrDrawSourceKind_Image) {
        *texels = source.rect;
        return cpuTextureFromImage(r.cpu->textures, source.image);
    }
    if (source.kind != PrDrawSourceKind_List)
        return nullptr;
    CpuTexture* texture = listTexture(r, source.list, source.rect, source.rect, scale);
    if (texture != nullptr)
        *texels = PrRect{0, 0, float(texture->levels.data[0].width), float(texture->levels.data[0].height)};
    return texture;
}

// Draws one coverage for every piece, in which shared edges cancel, and a paint that samples each pixel from the piece
// that its center falls in.
void drawNineSlice(Renderer& r, const PrDrawCommand& c) {
    const PrDrawTransform& x = r.transforms.last();
    const PrDrawSource& image = c.nineSlice.source;
    float width = prRectWidth(image.rect);
    float height = prRectHeight(image.rect);
    uint32_t count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{width, height}, c.nineSlice.destination, nullptr, 0);
    if (count == 0)
        return;
    Array<PrDrawImageRect>& pieces = r.pieces;
    pieces.clear();
    pieces.reserve(count);
    pieces.count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{width, height}, c.nineSlice.destination, pieces.data, count);
    // A list renders at the largest scale of any piece, and the pieces then sample its texels.
    float most = 0;
    for (uint32_t i = 0; i < pieces.count; ++i) {
        const PrDrawImageRect& p = pieces.data[i];
        most = prF32Max(most, prRectWidth(p.destination) / prF32Max(prRectWidth(p.source), 1e-6f));
        most = prF32Max(most, prRectHeight(p.destination) / prF32Max(prRectHeight(p.source), 1e-6f));
    }
    float device = prAffine2MaxScale(x.affine) * most;
    PrRect texels{};
    CpuTexture* texture = sourceTexture(r, image, PrVec2{device, device}, &texels);
    if (texture == nullptr)
        return;
    float kx = prRectWidth(texels) / width;
    float ky = prRectHeight(texels) / height;
    for (uint32_t i = 0; i < pieces.count; ++i) {
        PrRect& s = pieces.data[i].source;
        s = PrRect{
            texels.left + s.left * kx, texels.top + s.top * ky, texels.left + s.right * kx,
            texels.top + s.bottom * ky,
        };
    }
    CpuPaint paint{};
    CpuPlacement placement{};
    if (!makePaint(r, nullptr, c.color, c.nineSlice.destination, x, &paint) ||
            !placeFromUser(x, prAffine2Identity(), &placement))
        return;
    cpuPrepareMips(*texture, PrDrawSampling(c.sampling & PrDrawSamplingFlag_Kind));
    addImageStage(&paint, texture, texels, c.sampling, placement);
    paint.stages[paint.stageCount - 1].source.pieces = pieces.data;
    paint.stages[paint.stageCount - 1].source.pieceCount = pieces.count;
    prPathClear(r.shape);
    for (uint32_t i = 0; i < pieces.count; ++i)
        prPathAddRect(r.shape, pieces.data[i].destination);
    const PrPathPolyline& landed = land(x, r.shape, r.landed);
    PrIRect limit = prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    PrIRect box = polylineBox(landed, limit);
    if (prIRectIsEmpty(box))
        return;
    CpuCoverage shape = maskOver(r.coverage, box);
    rasterizeOver(r, landed, PrPathFillRule_NonZero, coverageOf(c.antialias), shape);
    composite(r, shape, paint, c.composite, c.blendMode);
}

// Replaces the target's pixels at the device point with the image's, ignoring all state.
void writePixels(Renderer& r, const PrDrawCommand& c) {
    const CpuTarget& t = r.target;
    const PrDrawImage* image = c.writePixels.image;
    PrIRect written{
        c.writePixels.origin.x, c.writePixels.origin.y,
        c.writePixels.origin.x + int32_t(prDrawImageExtent(image).x),
        c.writePixels.origin.y + int32_t(prDrawImageExtent(image).y),
    };
    r.target.drawn = prIRectUnion(r.target.drawn, prIRectIntersect(written, t.box));
    PrPixels pixels{t.data, t.stride, {prIRectWidth(t.box), prIRectHeight(t.box)}};
    prDrawImageWritePixels(image, pixels, t.format,
        PrIVec2{c.writePixels.origin.x - t.box.left, c.writePixels.origin.y - t.box.top});
}

// ---------------------------------------------------------------------------------------------------------------------
// Clips

void pushClipCoverage(Renderer& r, const PrPath* shape, const PrDrawTransform& placement, PrPathFillRule rule,
        bool antialias) {
    const PrPathPolyline& landed = land(placement, shape, r.landed);
    const Clip& outer = currentClip(r);
    PrIRect box = polylineBox(landed, outer.coverage.box);
    if (prIRectIsEmpty(box)) {
        r.clips.push(clipOfBox(PrIRect{0, 0, 0, 0}));
        return;
    }
    // A rectangle narrows the clip's rectangle, and keeps the enclosing mask, which is seen through the narrower box.
    if (cpuIsRectPolyline(landed)) {
        PrRect device = prPathPolylineBounds(&landed);
        if (!antialias)
            device = prRectFromIRect(prIRectRound(device));
        PrRect rect = prRectIntersect(outer.rect, device);
        PrIRect narrowed = prIRectIntersect(prIRectRoundOut(rect), outer.coverage.box);
        if (prRectIsEmpty(rect) || prIRectIsEmpty(narrowed)) {
            r.clips.push(clipOfBox(PrIRect{0, 0, 0, 0}));
            return;
        }
        CpuCoverage kept{outer.coverage.data, outer.coverage.stride, narrowed};
        if (kept.data != nullptr)
            kept.data += size_t(narrowed.top - outer.coverage.box.top) * kept.stride +
                size_t(narrowed.left - outer.coverage.box.left);
        r.clips.push(Clip{kept, rect, !prRectIsOnPixels(rect)});
        return;
    }
    size_t size = size_t(prIRectWidth(box)) * prIRectHeight(box);
    CpuCoverage mask{pooled(r.clipMasks, r.clips.count, size), size_t(prIRectWidth(box)), box};
    rasterizeOver(r, landed, rule, coverageOf(antialias), mask);
    if (outer.coverage.data != nullptr || outer.edges) {
        uint8_t scratch[kShadeSpan];
        uint32_t width = uint32_t(prIRectWidth(box));
        for (int32_t y = box.top; y < box.bottom; ++y) {
            uint8_t* row = mask.data + size_t(y - box.top) * mask.stride;
            for (uint32_t start = 0; start < width; start += kShadeSpan) {
                uint32_t count = width - start < kShadeSpan ? width - start : kShadeSpan;
                const uint8_t* c = clipSpan(outer, box.left + int32_t(start), y, count, scratch);
                if (c == nullptr)
                    continue;
                for (uint32_t i = 0; i < count; ++i)
                    row[start + i] = uint8_t(cpuDivide255(row[start + i] * c[i]));
            }
        }
    }
    r.clips.push(Clip{mask, outer.rect, false});
}

// ---------------------------------------------------------------------------------------------------------------------
// Groups

// Evaluates a brush's alpha over a box, for a filter node that reads a brush.
void brushAlpha(void* PR_NULLABLE context, const PrDrawBrush* brush, PrAffine2 transform, PrIRect box, float* out) {
    Renderer& r = *static_cast<Renderer*>(context);
    CpuPaint paint{};
    uint32_t width = prIRectWidth(box);
    size_t count = size_t(width) * prIRectHeight(box);
    if (!makePaint(r, brush, PrColor{1, 1, 1, 1}, prRectFromIRect(box), prDrawTransformFromAffine2(transform), &paint)) {
        prMemZero(out, count * sizeof(float));
        return;
    }
    float colors[kShadeSpan * 4];
    for (int32_t y = box.top; y < box.bottom; ++y) {
        float* row = out + size_t(y - box.top) * width;
        for (uint32_t start = 0; start < width; start += kShadeSpan) {
            uint32_t n = width - start < kShadeSpan ? width - start : kShadeSpan;
            cpuShade(paint, box.left + int32_t(start), y, n, colors);
            for (uint32_t x = 0; x < n; ++x)
                row[start + x] = colors[x * 4 + 3];
        }
    }
}

// Returns an image node's source texture, for a filter.
const CpuTexture* PR_NULLABLE filterSourceTexture(void* PR_NULLABLE context, const PrDrawSource* source, uint32_t width,
        uint32_t height, PrRect* texels) {
    Renderer& r = *static_cast<Renderer*>(context);
    PrVec2 scale{float(width) / prRectWidth(source->rect), float(height) / prRectHeight(source->rect)};
    return sourceTexture(r, *source, scale, texels);
}

CpuFilterInputs filterInputs(Renderer& r, const Frame& frame, const CpuTarget* backdrop) {
    CpuFilterInputs inputs{};
    inputs.transform = frame.xform.affine;
    inputs.bounds = frame.group->bounds;
    inputs.backdrop = backdrop;
    inputs.textures = &r.cpu->textures;
    inputs.brushAlpha = brushAlpha;
    inputs.sourceTexture = filterSourceTexture;
    inputs.context = &r;
    return inputs;
}

// Returns the bounds in user space of a box of target pixels under the transform. Returns an empty rect if the
// transform cannot be inverted.
PrRect userRectOf(const PrDrawTransform& x, PrIRect box) {
    PrAffine2 back{};
    if (!prAffine2Invert(x.affine, &back))
        return PrRect{0, 0, 0, 0};
    return prAffine2MapRect(back, prRectFromIRect(box));
}

// Returns the target pixels that a rectangle of user space covers.
PrIRect deviceBoxOf(Renderer& r, const PrDrawTransform& x, PrRect rect) {
    prPathClear(r.extra);
    prPathAddRect(r.extra, rect);
    const PrPathPolyline& landed = land(x, r.extra, r.landed);
    if (landed.pointCount == 0)
        return PrIRect{0, 0, 0, 0};
    return prIRectRoundOut(prPathPolylineBounds(&landed));
}

// Makes a paint of a target's pixels, sampled between pixels at the placement, over its box and transparent outside it.
// The caller resets the texture's levels. The texels are the target's own if it is RGBA with packed rows, and otherwise
// are copied into `storage`.
void targetPaint(const CpuTarget& target, Array<uint8_t>& storage, const CpuPlacement& placement,
        CpuTexture* texture, CpuPaint* paint) {
    uint32_t width = prIRectWidth(target.box);
    uint32_t height = prIRectHeight(target.box);
    const uint32_t bytes = cpuPixelBytes(target.format);
    size_t size = size_t(width) * height * bytes;
    PrPixelFormat format = target.format == PrPixelFormat_RgbaF16 ? PrPixelFormat_RgbaF16 : PrPixelFormat_Rgba8;
    CpuTextureLevel level{target.data, width, height, format};
    bool packed = target.format != PrPixelFormat_Bgra8 && target.stride == size_t(width) * bytes;
    if (!packed) {
        if (storage.capacity < size + 4) {
            storage.reset();
            storage.reserve(uint32_t(size + 4));
        }
        level.data = storage.data;
    }
    bool bgra = target.format == PrPixelFormat_Bgra8;
    for (uint32_t y = 0; y < height && !packed; ++y) {
        const uint8_t* row = cpuTargetRow(target, target.box.top + int32_t(y));
        uint8_t* out = level.data + size_t(y) * width * bytes;
        if (!bgra) {
            prMemCopy(out, row, size_t(width) * bytes);
            continue;
        }
        for (uint32_t x = 0; x < width; ++x) {
            out[x * 4] = row[x * 4 + 2];
            out[x * 4 + 1] = row[x * 4 + 1];
            out[x * 4 + 2] = row[x * 4];
            out[x * 4 + 3] = row[x * 4 + 3];
        }
    }
    texture->levels.push(level);
    *paint = CpuPaint{};
    paint->color[0] = paint->color[1] = paint->color[2] = paint->color[3] = 1;
    paint->stageCount = 1;
    CpuStage& stage = paint->stages[0];
    stage.placement = placement;
    stage.source.kind = SourceKind_Texture;
    stage.source.texture = texture;
    stage.source.texels = PrRect{0, 0, float(width), float(height)};
    stage.source.tiles.tile = prRectFromIRect(target.box);
    stage.source.sampling = PrDrawSampling_Linear;
}

// Returns `beneath` resampled into an offscreen's pixels over `box`, in the storage for the group at `index`. The
// placement maps each offscreen pixel to its position in `beneath`. The result is transparent where the placement
// cannot map it.
CpuTarget resampleUnder(Renderer& r, const CpuTarget& beneath, const CpuPlacement& placement, uint32_t index, PrIRect box) {
    const uint32_t bytes = cpuPixelBytes(beneath.format);
    size_t size = size_t(prIRectWidth(box)) * prIRectHeight(box) * bytes;
    CpuTarget under{pooled(r.unders, index, size), size_t(prIRectWidth(box)) * bytes, box, beneath.format, false, box};
    if (prIRectIsEmpty(beneath.box)) {
        prMemZero(under.data, size);
        return under;
    }
    CpuTexture texture{};
    CpuPaint paint{};
    targetPaint(beneath, r.copy, placement, &texture, &paint);
    uint32_t width = prIRectWidth(box);
    float colors[kShadeSpan * 4];
    for (int32_t y = box.top; y < box.bottom; ++y) {
        uint8_t* row = cpuTargetRow(under, y);
        for (uint32_t start = 0; start < width; start += kShadeSpan) {
            uint32_t n = width - start < kShadeSpan ? width - start : kShadeSpan;
            cpuShade(paint, box.left + int32_t(start), y, n, colors);
            for (uint32_t x = 0; x < n; ++x)
                cpuWritePixel(row + size_t(start + x) * bytes, under.format, colors + x * 4);
        }
    }
    texture.levels.reset();
    return under;
}

const CpuTarget& projectedUnder(Renderer& r, uint32_t index);

// Returns what a backdrop reads beneath the drawing into `top`, the target of the frames below `count`. That is `top`,
// unless the innermost group among those frames that has an offscreen is projected. A projected group is not a backdrop
// root, so its offscreen is then read over what lies beneath its plane.
CpuTarget seenThrough(Renderer& r, uint32_t count, const CpuTarget& top) {
    for (uint32_t i = count; i-- > 0;) {
        if (!r.frames.data[i].layered)
            continue;
        if (prDrawGroupIsBackdropRoot(r.frames.data[i].group))
            return top;
        const CpuTarget& under = projectedUnder(r, i);
        const uint32_t bytes = cpuPixelBytes(top.format);
        size_t size = size_t(prIRectWidth(top.box)) * prIRectHeight(top.box) * bytes;
        CpuTarget out{
            pooled(r.seen, i, size), size_t(prIRectWidth(top.box)) * bytes, top.box, top.format, false, top.box,
        };
        for (int32_t y = top.box.top; y < top.box.bottom; ++y) {
            const uint8_t* above = cpuTargetRow(top, y);
            const uint8_t* below = cpuTargetRow(under, y);
            uint8_t* row = cpuTargetRow(out, y);
            for (size_t x = 0; x < size_t(prIRectWidth(top.box)) * bytes; x += bytes) {
                if (top.format == PrPixelFormat_RgbaF16) {
                    float a[4];
                    float b[4];
                    cpuReadPixel(above + x, top.format, a);
                    cpuReadPixel(below + x, top.format, b);
                    for (uint32_t k = 0; k < 4; ++k)
                        a[k] += b[k] * (1 - a[3]);
                    cpuWritePixel(row + x, top.format, a);
                    continue;
                }
                uint32_t keep = 255 - above[x + 3];
                for (size_t k = 0; k < 4; ++k)
                    row[x + k] = uint8_t(above[x + k] + cpuDivide255(below[x + k] * keep));
            }
        }
        return out;
    }
    return top;
}

// Returns what lies beneath the projected group at `index`, resampled into its offscreen through the projection. It is
// computed the first time it is read, which is before anything draws over its parent again.
const CpuTarget& projectedUnder(Renderer& r, uint32_t index) {
    if (!r.frames.data[index].underReady) {
        CpuTarget beneath = seenThrough(r, index, r.frames.data[index].parent);
        Frame& frame = r.frames.data[index];
        CpuPlacement placement{};
        placement.kind = PlacementKind_Projective;
        placement.transform = prAffine2Identity();
        placement.unproject = frame.projection.layerToTarget;
        frame.under = resampleUnder(r, beneath, placement, index, frame.under.box);
        frame.underReady = true;
    }
    return r.frames.data[index].under;
}

// Opens a projected group's offscreen, which holds the visible part of its plane at the projection's scale. The
// contents draw into it flat.
void pushProjectedGroup(Renderer& r, Frame& frame) {
    const PrDrawGroup* g = frame.group;
    const PrDrawTransform& x = frame.xform;
    frame.projected = true;
    PrIRect visible = prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    PrIRect box{0, 0, 0, 0};
    float scale = 1;
    if (!prIRectIsEmpty(visible) && prDrawProjectionPlace(g->projection, x.affine, g->bounds, prRectFromIRect(visible),
            float(kListTextureMaxSize), &frame.projection)) {
        scale = frame.projection.scale;
        box = prIRectRoundOut(prAffine2MapRect(prAffine2Scale(PrVec2{scale, scale}), frame.projection.area));
    }
    if (prIRectIsEmpty(box))
        box = PrIRect{0, 0, 0, 0};
    frame.xform = prDrawTransformFromAffine2(prAffine2Scale(PrVec2{scale, scale}));
    const uint32_t bytes = cpuPixelBytes(r.target.format);
    size_t size = size_t(prIRectWidth(box)) * prIRectHeight(box) * bytes;
    frame.under = CpuTarget{nullptr, size_t(prIRectWidth(box)) * bytes, box, r.target.format, false, box};
    CpuTarget layer{
        pooled(r.layers, r.frames.count, size), size_t(prIRectWidth(box)) * bytes, box,
        r.target.format, g->knockout, PrIRect{0, 0, 0, 0},
    };
    prMemZero(layer.data, size);
    r.frames.push(frame);
    if (g->backdrop != nullptr) {
        layer.drawn = box;
        const CpuTarget& under = projectedUnder(r, r.frames.count - 1);
        CpuFilterInputs inputs = filterInputs(r, frame, &under);
        cpuApplyFilter(g->backdrop, inputs, layer);
    }
    r.transforms.push(frame.xform);
    r.target = layer;
    r.clips.push(clipOfBox(box));
}

// Returns the innermost group that draws into pixels of the host's, or null.
Frame* PR_NULLABLE hostFrameOf(Renderer& r) {
    for (uint32_t i = r.frames.count; i-- > 0;) {
        if (r.frames.data[i].host)
            return &r.frames.data[i];
    }
    return nullptr;
}

// Opens a group whose offscreen is the host's pixels, which keep what they held. The group's bounds map onto them, and
// its contents draw there in that space. A group that covers nothing draws into nothing.
void pushHostGroup(Renderer& r, Frame& frame) {
    const PrDrawGroup* g = frame.group;
    const PrDrawCpuTexture* texture = PR_POINTER_CAST(const PrDrawCpuTexture*, uintptr_t(g->layer->texture));
    PrUVec2 extent = g->layer->extent;
    float width = prRectWidth(g->bounds);
    float height = prRectHeight(g->bounds);
    frame.local = true;
    frame.host = true;
    frame.drawn = drawSharedClaimLayer(*g->layer);
    PrIRect box{0, 0, int32_t(extent.x), int32_t(extent.y)};
    bool covers = texture != nullptr && extent.x != 0 && extent.y != 0 && width > 0 && height > 0 &&
        texture->pixels.extent.x >= extent.x && texture->pixels.extent.y >= extent.y;
    if (!covers)
        box = PrIRect{0, 0, 0, 0};
    double sx = covers ? double(extent.x) / double(width) : 1;
    double sy = covers ? double(extent.y) / double(height) : 1;
    double left = double(g->bounds.left);
    double top = double(g->bounds.top);
    PrDAffine2 toLocal{sx, 0, 0, sy, -left * sx, -top * sy};
    PrDAffine2 fromLocal{1 / sx, 0, 0, 1 / sy, left, top};
    frame.localToDevice = prDrawTransformFromDAffine2(prDAffine2Concat(fromLocal, frame.xform.daffine));
    frame.xform = prDrawTransformFromDAffine2(toLocal);
    uint8_t* pixels = covers ? static_cast<uint8_t*>(texture->pixels.data) : nullptr;
    CpuTarget layer{
        pixels, covers ? texture->pixels.stride : 0, box,
        covers ? texture->format : r.target.format, g->knockout, box,
    };
    r.frames.push(frame);
    r.transforms.push(frame.xform);
    r.target = layer;
    r.clips.push(clipOfBox(box));
}

void pushGroup(Renderer& r, const PrDrawCommand& c) {
    const PrDrawGroup* g = c.group;
    PrDrawTransform x = r.transforms.last();
    Frame frame{};
    frame.group = g;
    frame.composite = c.composite;
    frame.blendMode = c.blendMode;
    frame.xform = x;
    frame.clipDepth = r.clips.count;
    frame.parent = r.target;
    frame.layered = prDrawGroupHasEffect(g);
    if (!frame.layered) {
        // Contents of a revision that the host's pixels already hold have been drawn into them, and contents past the
        // layer's revision are not claimed.
        if (Frame* host = g->revision != 0 ? hostFrameOf(r) : nullptr)
            frame.hidden = g->revision <= host->drawn || g->revision > host->group->layer->revision;
        if (frame.hidden)
            r.clips.push(clipOfBox(PrIRect{0, 0, 0, 0}));
        r.frames.push(frame);
        return;
    }
    if (g->projection != nullptr) {
        pushProjectedGroup(r, frame);
        return;
    }
    if (g->layer != nullptr) {
        pushHostGroup(r, frame);
        return;
    }
    // What lies outside the offscreen composites as transparent, so the visible box reaches the clip's box, whatever
    // the operator.
    const Clip& outer = currentClip(r);
    PrIRect visible = prIRectIntersect(outer.coverage.box, r.target.box);
    bool bounded = !prRectIsEmpty(g->bounds);
    if (bounded)
        visible = prIRectIntersect(visible, deviceBoxOf(r, x, g->bounds));
    PrIRect box = visible;
    // A filter computes in the group's space. If the transform rotates or skews that space, the group draws into an
    // offscreen in the group's space. The offscreen has equal scale on both axes, at the device's resolution, over what
    // lies beneath resampled into that space.
    bool turned = !(x.affine.b == 0 && x.affine.c == 0);
    PrRect area = bounded ? g->bounds : userRectOf(x, visible);
    if (bounded)
        area = prRectIntersect(area, userRectOf(x, visible));
    float scale = prAffine2MaxScale(x.affine);
    bool filters = g->filter != nullptr || g->backdrop != nullptr;
    // An offscreen too large for what its filters reach draws at a lower resolution, in the group's space.
    auto reachAt = [&](float at) {
        int32_t reach = g->filter != nullptr ? cpuFilterReach(g->filter, at) : 0;
        if (g->backdrop != nullptr && cpuFilterReach(g->backdrop, at) > reach)
            reach = cpuFilterReach(g->backdrop, at);
        return reach;
    };
    float resolution = filters && scale > 0 ? prDrawLayerResolution(float(reachAt(scale))) : 1;
    if (filters && (turned || resolution < 1) && !prRectIsEmpty(area) && scale > 0) {
        scale *= resolution;
        PrAffine2 toLocal = prAffine2Scale(PrVec2{scale, scale});
        PrDAffine2 fromLocal = prDAffine2FromAffine2(prAffine2Scale(PrVec2{1 / scale, 1 / scale}));
        frame.local = true;
        frame.localToDevice = prDrawTransformFromDAffine2(prDAffine2Concat(fromLocal, x.daffine));
        frame.xform = prDrawTransformFromAffine2(toLocal);
        box = prIRectRoundOut(prAffine2MapRect(toLocal, area));
        if (g->filter != nullptr)
            box = prIRectOutset(box, cpuFilterReach(g->filter, scale));
        if (bounded)
            box = prIRectIntersect(box, prIRectRoundOut(prAffine2MapRect(toLocal, g->bounds)));
        int32_t reach = reachAt(scale);
        CpuPlacement placement{};
        placement.transform = frame.localToDevice.affine;
        CpuTarget beneath = seenThrough(r, r.frames.count, frame.parent);
        frame.under = resampleUnder(r, beneath, placement, r.frames.count, prIRectOutset(box, reach));
        r.transforms.push(frame.xform);
    } else {
        if (g->filter != nullptr)
            box = prIRectOutset(box, cpuFilterReach(g->filter, scale));
        if (bounded)
            box = prIRectIntersect(box, deviceBoxOf(r, x, g->bounds));
    }
    if (prIRectIsEmpty(box))
        box = PrIRect{0, 0, 0, 0};
    const uint32_t bytes = cpuPixelBytes(r.target.format);
    size_t size = size_t(prIRectWidth(box)) * prIRectHeight(box) * bytes;
    CpuTarget layer{
        pooled(r.layers, r.frames.count, size), size_t(prIRectWidth(box)) * bytes, box,
        r.target.format, g->knockout, PrIRect{0, 0, 0, 0},
    };
    CpuTarget under = frame.local ? frame.under : g->backdrop != nullptr ? seenThrough(r, r.frames.count, r.target) : r.target;
    // A backdrop starts from what lies beneath, which covers the whole offscreen.
    if (g->backdrop == nullptr || !prIRectContains(under.box, box))
        prMemZero(layer.data, size);
    if (g->backdrop != nullptr) {
        // What lies beneath, filtered, goes under the contents, clipped in the same way as the group. The clip applies
        // here, or when the offscreen composites if it is in the group's space.
        cpuCopyTarget(under, layer);
        layer.drawn = box;
        CpuFilterInputs inputs = filterInputs(r, frame, &under);
        cpuApplyFilter(g->backdrop, inputs, layer);
        if (!frame.local)
            maskByClip(layer, outer);
    }
    r.frames.push(frame);
    r.target = layer;
    r.clips.push(clipOfBox(box));
}

// Multiplies the offscreen by the group's mask, evaluated in the group's space.
void applyMask(Renderer& r, const Frame& frame, const CpuTarget& layer) {
    const PrDrawMask* mask = frame.group->mask;
    CpuPaint paint{};
    PrRect bounds = frame.group->bounds;
    if (prRectIsEmpty(bounds))
        bounds = userRectOf(frame.xform, layer.box);
    bool paints = makePaint(r, &mask->brush, PrColor{1, 1, 1, mask->brush.opacity}, bounds, frame.xform, &paint);
    const PrIRect& drawn = layer.drawn;
    uint32_t width = uint32_t(prIRectWidth(drawn));
    float colors[kShadeSpan * 4];
    for (int32_t y = drawn.top; y < drawn.bottom; ++y) {
        uint8_t* row = cpuTargetPixel(layer, drawn.left, y);
        for (uint32_t start = 0; start < width; start += kShadeSpan) {
            uint32_t count = width - start < kShadeSpan ? width - start : kShadeSpan;
            int32_t x0 = drawn.left + int32_t(start);
            if (paints)
                cpuShade(paint, x0, y, count, colors);
            for (uint32_t i = 0; i < count; ++i) {
                float m = paints ? cpuMaskValue(mask, colors + i * 4) : 0;
                if (mask->invert)
                    m = 1 - m;
                uint32_t scale = uint32_t(prF32Clamp(m, 0, 1) * 255 + 0.5f);
                cpuScalePixel(row + size_t(start + i) * cpuPixelBytes(layer.format), layer.format, scale);
            }
        }
    }
}

// Composites an offscreen in the group's space onto the target through the rest of the group's transform, sampled
// between its pixels as `sampling` says.
void compositeResampled(Renderer& r, const Frame& frame, const CpuTarget& layer,
        PrDrawSampling sampling = PrDrawSampling_Linear) {
    const PrDrawTransform& toDevice = frame.localToDevice;
    if (prIRectIsEmpty(layer.drawn))
        return;
    CpuPlacement placement{};
    if (!placeFromUser(toDevice, prAffine2Identity(), &placement))
        return;
    CpuTexture texture{};
    CpuPaint paint{};
    targetPaint(layer, r.copy, placement, &texture, &paint);
    paint.stages[0].source.sampling = sampling;
    prPathClear(r.shape);
    prPathAddRect(r.shape, prRectFromIRect(layer.drawn));
    const PrPathPolyline& landed = land(toDevice, r.shape, r.landed);
    PrIRect limit;
    if (prDrawCompositeIsUnbounded(frame.composite))
        limit = currentClip(r).coverage.box;
    else
        limit = prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    PrIRect box = polylineBox(landed, limit);
    if (!prIRectIsEmpty(box) || prDrawCompositeIsUnbounded(frame.composite)) {
        CpuCoverage shape = maskOver(r.coverage, box);
        rasterizeOver(r, landed, PrPathFillRule_NonZero, PrPathCoverage_Area, shape);
        composite(r, shape, paint, frame.composite, frame.blendMode, true);
    }
    texture.levels.reset();
}

// Composites a projected group's offscreen onto the target where the projection lands, sampling the offscreen at each
// target pixel's point of the plane.
void compositeProjected(Renderer& r, const Frame& frame, const CpuTarget& layer) {
    if (prIRectIsEmpty(layer.drawn))
        return;
    CpuPlacement placement{};
    placement.kind = PlacementKind_Projective;
    placement.transform = prAffine2Identity();
    placement.unproject = frame.projection.targetToLayer;
    CpuTexture texture{};
    CpuPaint paint{};
    targetPaint(layer, r.copy, placement, &texture, &paint);
    // Farther parts of the plane sample smaller levels.
    paint.stages[0].source.sampling = PrDrawSampling_Mipmap;
    cpuPrepareMips(texture, PrDrawSampling_Mipmap);
    float scale = frame.projection.scale;
    PrRect plane = prAffine2MapRect(prAffine2Scale(PrVec2{scale, scale}), frame.projection.area);
    PrRect drawn = prRectIntersect(plane, prRectFromIRect(layer.drawn));
    prPathPolylineClear(&r.landed);
    if (!prRectIsEmpty(drawn))
        prDrawProjectionQuad(&frame.projection, drawn, &r.landed);
    bool unbounded = prDrawCompositeIsUnbounded(frame.composite);
    PrIRect limit =
        unbounded ? currentClip(r).coverage.box : prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    PrIRect box = polylineBox(r.landed, limit);
    if (!prIRectIsEmpty(box) || unbounded) {
        CpuCoverage shape = maskOver(r.coverage, box);
        rasterizeOver(r, r.landed, PrPathFillRule_NonZero, PrPathCoverage_Area, shape);
        composite(r, shape, paint, frame.composite, frame.blendMode, true);
    }
    for (uint32_t i = 1; i < texture.levels.count; ++i)
        prMemoryFree(texture.levels.data[i].data);
    texture.levels.reset();
}

void popGroup(Renderer& r) {
    Frame frame = r.frames.data[--r.frames.count];
    if (!frame.layered) {
        if (frame.hidden)
            r.clips.pop();
        return;
    }
    while (r.clips.count > frame.clipDepth)
        r.clips.pop();
    CpuTarget layer = r.target;
    r.target = frame.parent;
    const PrDrawGroup* g = frame.group;
    if (frame.host) {
        r.transforms.pop();
        compositeResampled(r, frame, layer, g->layer->sampling);
        return;
    }
    if (g->filter != nullptr) {
        // The frame's own entry is still in place above the frames' count, so a projected group's beneath can be read.
        CpuTarget beneath;
        if (frame.projected)
            beneath = projectedUnder(r, r.frames.count);
        else if (frame.local)
            beneath = frame.under;
        else
            beneath = seenThrough(r, r.frames.count, frame.parent);
        CpuFilterInputs inputs = filterInputs(r, frame, &beneath);
        cpuApplyFilter(g->filter, inputs, layer);
        layer.drawn = layer.box;
    }
    if (g->mask != nullptr)
        applyMask(r, frame, layer);
    if (g->opacity < 1) {
        uint32_t scale = uint32_t(prF32Clamp(g->opacity, 0, 1) * 255 + 0.5f);
        for (int32_t y = layer.drawn.top; y < layer.drawn.bottom; ++y) {
            uint8_t* row = cpuTargetPixel(layer, layer.drawn.left, y);
            for (uint32_t i = 0; i < prIRectWidth(layer.drawn); ++i)
                cpuScalePixel(row + size_t(i) * cpuPixelBytes(layer.format), layer.format, scale);
        }
    }
    CpuTarget parent = r.target;
    // A knockout target treats a group in the same way as any other source over its children.
    r.target.knockout = false;
    if (frame.projected) {
        r.transforms.pop();
        compositeProjected(r, frame, layer);
    } else if (frame.local) {
        r.transforms.pop();
        compositeResampled(r, frame, layer);
    } else {
        compositeLayer(r, layer, frame.composite, frame.blendMode);
    }
    r.target.knockout = parent.knockout;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lists

CpuTexture* listTexture(Renderer& r, const PrDrawList* drawn, PrRect area, PrRect tile, PrVec2 scale) {
    PrUVec2 layer = prDrawLayerExtent(PrVec2{prRectWidth(tile), prRectHeight(tile)}, scale, kListTextureMaxSize);
    uint32_t width = layer.x;
    uint32_t height = layer.y;
    if (width == 0)
        return nullptr;
    // A list draws in the range of the target that it draws into.
    PrPixelFormat format = r.target.format == PrPixelFormat_RgbaF16 ? PrPixelFormat_RgbaF16 : PrPixelFormat_Rgba8;
    CpuTexture* texture = cpuListTexture(r.cpu->textures, prDrawListId(drawn), area, width, height, format);
    if (texture->levels.data[0].data != nullptr && texture->ready)
        return texture;
    CpuTextureLevel& level = texture->levels.data[0];
    prMemZero(level.data, size_t(width) * height * cpuPixelBytes(format));
    CpuTarget target{
        level.data,
        size_t(width) * cpuPixelBytes(format),
        PrIRect{0, 0, int32_t(width), int32_t(height)},
        format,
        false,
        PrIRect{0, 0, 0, 0},
    };
    Renderer& nested = beginRender(r.cpu, r.depth + 1, target);
    nested.transforms.data[0] =
        prDrawTransformFromAffine2(prAffine2Concat(prAffine2Translate(PrVec2{-area.left, -area.top}),
            prAffine2Scale(PrVec2{float(width) / prRectWidth(area), float(height) / prRectHeight(area)})));
    runCommands(nested, prDrawListCommands(drawn), prDrawListCommandCount(drawn));
    endRender(nested);
    texture->ready = true;
    return texture;
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

// A border piece's context: the renderer and the command being lowered.
struct BorderDraw {
    Renderer* r;
    const PrDrawCommand* command;
};

void drawBorderPiece(void* PR_NULLABLE context, const PrDrawBorderPiece* piece,
        const PrPathPolyline* shape, const PrPathPolyline* PR_NULLABLE cut) {
    BorderDraw& d = *static_cast<BorderDraw*>(context);
    Renderer& r = *d.r;
    const PrDrawCommand& c = *d.command;
    const PrDrawTransform& x = r.transforms.last();
    const PrDrawBorderSide& side = c.border->sides[piece->side];
    CpuPaint paint{};
    if (!makePaint(r, side.brush, piece->color, c.border->shape.rect, x, &paint))
        return;
    prPathPolylineClear(&r.landed);
    prPathPolylineAddPolyline(&r.landed, shape, x.affine);
    const PrPathPolyline& landed = r.landed;
    PrIRect box = polylineBox(landed, prIRectIntersect(currentClip(r).coverage.box, r.target.box));
    if (prIRectIsEmpty(box))
        return;
    CpuCoverage coverage = maskOver(r.coverage, box);
    rasterizeOver(r, landed, piece->rule, coverageOf(c.antialias), coverage);
    if (cut != nullptr) {
        CpuCoverage wedge = maskOver(r.second, box);
        prPathPolylineClear(&r.cut);
        prPathPolylineAddPolyline(&r.cut, cut, x.affine);
        rasterizeOver(r, r.cut, PrPathFillRule_NonZero, coverageOf(c.antialias), wedge);
        size_t size = size_t(prIRectWidth(box)) * prIRectHeight(box);
        for (size_t k = 0; k < size; ++k)
            coverage.data[k] = uint8_t(cpuDivide255(coverage.data[k] * wedge.data[k]));
    }
    composite(r, coverage, paint, c.composite, c.blendMode);
}

// Draws a border whose sides color one ring (prDrawBorderSides) as one area, whose pixels take the colors of the sides
// that they lie on. Returns false for any other border.
bool drawSidedRing(Renderer& r, const PrDrawCommand& c) {
    const PrDrawBorder& border = *c.border;
    PrDrawBorderSides sides{};
    if (!prDrawBorderSides(&border, &sides))
        return false;
    for (uint32_t k = 0; k < 4; ++k) {
        for (PrColor& color : sides.colors[k]) {
            float a = prF32Clamp(color.a, 0, 1);
            color = PrColor{color.r * a, color.g * a, color.b * a, a};
        }
    }
    const PrDrawTransform& x = r.transforms.last();
    CpuPaint paint{};
    makePaint(r, nullptr, PrColor{1, 1, 1, 1}, border.shape.rect, x, &paint);
    CpuStage& stage = paint.stages[paint.stageCount++];
    if (!placeFromUser(x, prAffine2Identity(), &stage.placement))
        return true;
    stage.source.kind = SourceKind_Sides;
    stage.source.sides = &sides;
    prPathClear(r.shape);
    prPathAddCornerRect(r.shape, prDrawBorderContour(&border, 0));
    PrCornerRect within = prDrawBorderContour(&border, 1);
    if (!prRectIsEmpty(within.rect))
        prPathAddCornerRect(r.shape, within);
    const PrPathPolyline& landed = land(x, r.shape, r.landed);
    PrIRect box = polylineBox(landed, prIRectIntersect(currentClip(r).coverage.box, r.target.box));
    if (prIRectIsEmpty(box))
        return true;
    CpuCoverage coverage = maskOver(r.coverage, box);
    rasterizeOver(r, landed, PrPathFillRule_EvenOdd, coverageOf(c.antialias), coverage);
    composite(r, coverage, paint, c.composite, c.blendMode);
    return true;
}

void drawBorder(Renderer& r, const PrDrawCommand& c) {
    float scale = prAffine2MaxScale(r.transforms.last().affine);
    if (!(scale > 0) || drawSidedRing(r, c))
        return;
    BorderDraw d{&r, &c};
    prDrawBorderLower(c.border, kTolerance / scale, drawBorderPiece, &d);
}

void drawShadow(Renderer& r, const PrDrawCommand& c) {
    const PrDrawTransform& x = r.transforms.last();
    CpuPaint paint{};
    if (!makePaint(r, c.brush, c.color, prDrawFillBounds(&c), x, &paint))
        return;
    const PrDrawShadowKind kind =
        c.kind == PrDrawCommandKind_InnerShadow ? PrDrawShadowKind_Inner : PrDrawShadowKind_Drop;
    PrDrawShadowGeometry g = prDrawShadowGeometry(c.shadow, kind);
    // The blur is a Gaussian in the shadow's space. Its sigma and offset are scaled into target pixels unless they are
    // already measured in target pixels.
    float scale = c.shadow->nonScaling ? 1 : prAffine2MaxScale(x.affine);
    float sigma = g.sigma * scale;
    PrDrawTransform shifted = x;
    if (c.shadow->nonScaling) {
        PrDAffine2 move = prDAffine2FromAffine2(prAffine2Translate(c.shadow->offset));
        shifted = prDrawTransformFromDAffine2(prDAffine2Concat(x.daffine, move));
    }
    prPathClear(r.shape);
    prPathAddCornerRect(r.shape, g.shape);
    const PrPathPolyline& shadow = land(shifted, r.shape, r.landed);
    prPathClear(r.extra);
    prPathAddCornerRect(r.extra, c.shadow->shape);
    const PrPathPolyline& caster = land(x, r.extra, r.cut);
    int32_t reach = int32_t(prF32Min(prF32Ceil(sigma * 4), float(1 << 28)));
    PrIRect limit = prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    PrIRect box;
    if (kind == PrDrawShadowKind_Inner) {
        box = polylineBox(caster, limit);
    } else {
        PrIRect everywhere{INT32_MIN / 2, INT32_MIN / 2, INT32_MAX / 2, INT32_MAX / 2};
        box = prIRectIntersect(prIRectOutset(polylineBox(shadow, everywhere), reach), limit);
    }
    if (prIRectIsEmpty(box))
        return;
    CpuCoverage shape = maskOver(r.coverage, box);
    prDrawShadowCoverage(r.rasterizer, &shadow, &caster, kind, sigma, coverageOf(c.antialias),
        PrIVec2{box.left, box.top}, PrPixels{shape.data, shape.stride, {prIRectWidth(box), prIRectHeight(box)}});
    composite(r, shape, paint, c.composite, c.blendMode);
}

void drawDecoration(Renderer& r, const PrDrawCommand& c) {
    const PrDrawTransform& x = r.transforms.last();
    CpuPaint paint{};
    if (!makePaint(r, c.brush, c.color, prDrawFillBounds(&c), x, &paint))
        return;
    float scale = prAffine2MaxScale(x.affine);
    if (!(scale > 0))
        return;
    prPathPolylineClear(&r.cut);
    prDrawDecorationOutline(c.decoration, kTolerance / scale, &r.cut);
    prPathPolylineClear(&r.landed);
    prPathPolylineAddPolyline(&r.landed, &r.cut, x.affine);
    const PrPathPolyline& landed = r.landed;
    PrIRect limit = prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    PrIRect box = polylineBox(landed, limit);
    if (prIRectIsEmpty(box))
        return;
    CpuCoverage shape = maskOver(r.coverage, box);
    rasterizeOver(r, landed, PrPathFillRule_NonZero, coverageOf(c.antialias), shape);
    composite(r, shape, paint, c.composite, c.blendMode);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Plays the list of a glyph that paints itself, at the glyph's position in the run.
void drawColorGlyph(Renderer& r, const PrDrawCommand& c, PrDrawList* drawn, uint32_t index) {
    PrDrawGroup group{};
    PrDrawCommand commands[kPrDrawGlyphMaxCommands];
    runCommands(r, commands, prDrawGlyphListCommands(&c, index, drawn, &group, commands));
}

// Draws a glyph run. Each glyph composites on its own, and a filled color glyph uses its own colors.
void drawGlyphs(Renderer& r, const PrDrawCommand& c) {
    const PrFontGlyphRun* run = c.glyphs;
    StrokeMap map{};
    if (mapsStroke(c) && !measureStroke(r, c, &map))
        return;
    PrPathMeasure* measure = map.measure;
    CpuPaint paint{};
    if (!makePaint(r, c.brush, c.color, prDrawFillBounds(&c), r.transforms.last(), &paint,
            measure != nullptr ? &map.unitPixels : nullptr)) {
        prPathMeasureDestroy(measure);
        return;
    }
    if (measure != nullptr)
        mapStroke(c, map, &paint);
    PrIRect limit = prIRectIntersect(currentClip(r).coverage.box, r.target.box);
    PrColor foreground = c.color;
    foreground.a = 1;
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        uint16_t glyph = run->glyphs[i].id;
        if (c.stroke == nullptr) {
            float pixelsPerEm = prDrawGlyphPixelsPerEm(run, i, r.transforms.last().affine);
            PrDrawList* drawn =
                prDrawGlyphCacheList(r.cpu->glyphs, run->font, glyph, pixelsPerEm, &run->palette, foreground);
            if (drawn != nullptr) {
                drawColorGlyph(r, c, drawn, i);
                continue;
            }
        }
        // With glyph phases, a glyph drawn at a phase of a target pixel is outlined at its own origin and placed at
        // that point, as an atlas rasterizes it, so that it covers the same pixels wherever its run puts it within the
        // phase.
        CpuCoverage shape{};
        const PrDrawTransform& x = r.transforms.last();
        prPathClear(r.shape);
        PrVec2 exact = prAffine2MapPoint(x.affine, run->glyphs[i].position);
        PrVec2 drawn = r.cpu->glyphPhases ? prDrawGlyphPlace(run, i, x.affine).origin : exact;
        bool phased = drawn.x != exact.x || drawn.y != exact.y;
        bool outlined = false;
        if (phased) {
            PrFontGlyph alone = run->glyphs[i];
            alone.position = PrVec2{0, 0};
            PrFontGlyphRun single = *run;
            single.glyphs = &alone;
            single.glyphCount = 1;
            single.transforms = run->transforms != nullptr ? &run->transforms[i] : nullptr;
            PrAffine2 place = x.affine;
            place.e = drawn.x;
            place.f = drawn.y;
            outlined = prDrawGlyphPath(&single, 0, &place, r.shape);
            r.transforms.push(prDrawTransformFromAffine2(place));
        } else {
            outlined = prDrawGlyphPath(run, i, &x.affine, r.shape);
        }
        if (outlined && pathCoverage(r, c, limit, &shape))
            composite(r, shape, paint, c.composite, c.blendMode);
        if (phased)
            r.transforms.pop();
    }
    prPathMeasureDestroy(measure);
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

void runCommands(Renderer& r, const PrDrawCommand* commands, uint32_t count) {
    PrDrawSnapped snapped;
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = *prDrawTransformSnap(&r.transforms.last(), &commands[i], &snapped);
        if (prDrawTransformDrawsNothing(&r.transforms.last(), &c)) {
            // A push still opens a scope that its pop closes: a clip that lets nothing through, or a group with no
            // offscreen.
            if (c.kind >= PrDrawCommandKind_PushClipRect && c.kind <= PrDrawCommandKind_PushClipPath)
                r.clips.push(clipOfBox(PrIRect{0, 0, 0, 0}));
            else if (c.kind == PrDrawCommandKind_PushGroup)
                r.frames.push(Frame{});
            continue;
        }
        switch (c.kind) {
            case PrDrawCommandKind_Clear:
                clearTarget(r, c.color);
                break;
            case PrDrawCommandKind_Border:
                drawBorder(r, c);
                break;
            case PrDrawCommandKind_DropShadow:
            case PrDrawCommandKind_InnerShadow:
                drawShadow(r, c);
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
            case PrDrawCommandKind_Embed:
                r.transforms.push(prDrawTransformMoved(&r.transforms.last(), c.embed.offset));
                runCommands(r, prDrawListCommands(c.embed.list), prDrawListCommandCount(c.embed.list));
                r.transforms.pop();
                break;
            case PrDrawCommandKind_PushTransformAffine2: {
                PrDrawTransform next = prDrawTransformPush(&r.transforms.last(), &c);
                r.transforms.push(next);
                break;
            }
            case PrDrawCommandKind_PopTransform:
                r.transforms.pop();
                break;
            case PrDrawCommandKind_PushClipTargetRect:
                prPathClear(r.shape);
                prPathAddRect(r.shape, c.rect);
                pushClipCoverage(r, r.shape, prDrawTransformFromAffine2(prAffine2Identity()), PrPathFillRule_NonZero,
                    c.antialias);
                break;
            case PrDrawCommandKind_PushClipRect:
            case PrDrawCommandKind_PushClipCornerRect:
            case PrDrawCommandKind_PushClipPath:
                prPathClear(r.shape);
                prDrawShapePath(&c, r.shape);
                pushClipCoverage(r, r.shape, r.transforms.last(), c.rule, c.antialias);
                break;
            case PrDrawCommandKind_PopClip:
                r.clips.pop();
                break;
            case PrDrawCommandKind_PushGroup:
                pushGroup(r, c);
                break;
            case PrDrawCommandKind_PopGroup:
                popGroup(r);
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
    CpuBackend* cpu = static_cast<CpuBackend*>(renderer);
    cpuDestroyTextures(cpu->textures);
    prDrawGlyphCacheDestroy(cpu->glyphs);
    for (uint32_t i = 0; i < cpu->renderers.count; ++i)
        destroyRenderer(cpu->renderers.data[i]);
    cpu->renderers.reset();
    prMemoryFree(cpu);
}

void destroyFrame(PrDrawRenderer*, PrDrawFrame* frame) {
    prMemoryFree(frame);
}

// Renders the list into the pixels. A backdrop reads what the pixels held as well as what the list drew.
void renderPixels(PrDrawRenderer* renderer, PrPixels pixels, PrPixelFormat format, const PrDrawList* list) {
    CpuBackend* cpu = static_cast<CpuBackend*>(renderer);
    PrIRect whole{0, 0, int32_t(pixels.extent.x), int32_t(pixels.extent.y)};
    CpuTarget target{pixels.data, pixels.stride, whole, format, false, whole};
    cpuBeginTextures(cpu->textures);
    prDrawGlyphCacheBegin(cpu->glyphs);
    for (uint32_t i = 0; i < cpu->renderers.count; ++i)
        cpu->renderers.data[i]->used = false;
    Renderer& r = beginRender(cpu, 0, target);
    runCommands(r, prDrawListCommands(list), prDrawListCommandCount(list));
    endRender(r);
    // Only renderers for the depths that nesting reached are kept, deepest last.
    while (!cpu->renderers.last()->used)
        destroyRenderer(cpu->renderers.data[--cpu->renderers.count]);
    cpuEndTextures(cpu->textures);
    prDrawGlyphCacheEnd(cpu->glyphs);
}

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    CpuFrame* frame = allocZeroed<CpuFrame>();
    frame->renderer = renderer;
    return frame;
}

// Keeps the list, which renders once there are pixels.
PrStatus prepare(PrDrawRenderer*, PrDrawFrame* frame, const PrDrawList* list, const PrDrawTarget*) {
    static_cast<CpuFrame*>(frame)->list = list;
    return PrStatus_Ok;
}

void renderFrame(PrDrawRenderer* renderer, PrDrawFrame* frame, PrPixels pixels, PrPixelFormat format) {
    renderPixels(renderer, pixels, format, static_cast<CpuFrame*>(frame)->list);
}
} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawCpuOptions prDrawCpuOptionsDefault(void) {
    return PrDrawCpuOptions{.glyphDecoder = nullptr, .glyphPhases = false};
}

PrDrawRenderer* prDrawCpuCreate(const PrDrawCpuOptions* PR_NULLABLE options) {
    CpuBackend* cpu = allocZeroed<CpuBackend>();
    PrDrawCpuOptions o = options != nullptr ? *options : prDrawCpuOptionsDefault();
    cpu->glyphs = prDrawGlyphCacheCreate(o.glyphDecoder);
    cpu->glyphPhases = o.glyphPhases;
    cpu->destroy = destroyBackend;
    cpu->frameCreate = createFrame;
    cpu->frameDestroy = destroyFrame;
    cpu->framePrepare = prepare;
    cpu->frameRenderPixels = renderFrame;
    cpu->rendersHalfFloat = true;
    return cpu;
}
