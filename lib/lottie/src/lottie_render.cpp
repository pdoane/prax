// Drawing a Lottie animation's compositions and layers.
//
// lottie-spec - https://lottie.github.io/lottie-spec/
//
// A layer is drawn inside a group for its blend mode and matte. Within that group, the layer is drawn under its own and
// its parents' transforms, in a group for its opacity, clipped to its size if it is a precomp, and masked.

#include "lottie/src/lottie_render.h"

#include "pr/draw_filter.h"
#include "pr/float.h"
#include "pr/path_polyline.h"

#include "lottie/src/lottie_effect.h"
#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_shape.h"
#include "lottie/src/lottie_text.h"

namespace pr_impl {

namespace {

// How far a mask's flattened outline may lie from its curves, in the layer's units.
constexpr float kMaskTolerance = 0.05f;

// Returns true if the layer is in its composition at `frame`: from its in point up to its out point.
bool inRange(const LottieLayer& layer, double frame) {
    return layer.inPoint <= frame && frame < layer.outPoint;
}

// Returns the layer's transform at `frame`, in its composition's time, composed with the transforms of its parents, in
// three dimensions. A layer's keyframes are in its composition's time, regardless of its start time.
PrMatrix4 layerChain(const LottieRender& r, const LottieComp& comp, uint32_t index, double frame) {
    const LottieAnimation& a = *r.a;
    PrMatrix4 m = prMatrix4Identity();
    // A parent chain longer than the layer count is a loop, and the walk stops.
    for (uint32_t n = 0; index != kLottieNone && n <= comp.count; ++n) {
        const LottieLayer& layer = a.layers.data[comp.layers + index];
        const LottieTransform& t = a.transforms.data[layer.transform];
        PrMatrix4 own = lottieTransformMatrix4(a, t, frame, layer.autoOrient);
        m = prMatrix4Concat(&m, &own);
        index = layer.parent;
    }
    return m;
}

// Returns the layer's transform flattened onto the composition's plane, as a layer draws without a camera.
PrAffine2 layerMatrix(const LottieRender& r, const LottieComp& comp, uint32_t index, double frame) {
    PrMatrix4 chain = layerChain(r, comp, index, frame);
    PrAffine2 flat = prAffine2Identity();
    prAffine2FromMatrix4(&chain, &flat);
    return flat;
}

// Writes the projection of the composition's active camera at `frame` to `out`, from the composition's 3D space to its
// plane, for the composition played at `extent`. Returns false if no camera is in range. The active camera is the
// topmost camera layer in range. Its view is composed with the inverse of its parents' transforms, and its perspective
// looks at the composition's center from its zoom.
bool cameraProjection(const LottieRender& r, const LottieComp& comp, double frame, PrVec2 extent, PrMatrix4* out) {
    const LottieAnimation& a = *r.a;
    for (uint32_t i = 0; i < comp.count; ++i) {
        const LottieLayer& camera = a.layers.data[comp.layers + i];
        if (camera.kind != LottieLayerKind_Camera || camera.hidden || !inRange(camera, frame))
            continue;
        float zoom = lottieScalar(a, camera.zoom, frame, 0);
        PrMatrix4 view = lottieCameraView(a, a.transforms.data[camera.transform], frame, extent, zoom);
        PrMatrix4 parents;
        if (camera.parent != kLottieNone)
            parents = layerChain(r, comp, camera.parent, frame);
        else
            parents = prMatrix4Identity();
        PrMatrix4 toParents = prMatrix4Identity();
        prMatrix4Invert(&parents, &toParents);
        PrMatrix4 toCenter = prMatrix4Translate(PrVec3{-extent.x / 2, -extent.y / 2, 0});
        PrMatrix4 perspective = prMatrix4Perspective(zoom);
        PrMatrix4 fromCenter = prMatrix4Translate(PrVec3{extent.x / 2, extent.y / 2, 0});
        PrMatrix4 m = prMatrix4Concat(&toParents, &view);
        m = prMatrix4Concat(&m, &toCenter);
        m = prMatrix4Concat(&m, &perspective);
        *out = prMatrix4Concat(&m, &fromCenter);
        return true;
    }
    return false;
}

// Returns the depth of the layer's anchor in the camera's space, before perspective: larger is nearer the eye.
float cameraDepth(const LottieRender& r, const LottieComp& comp, uint32_t index, double frame,
        const PrMatrix4& projection) {
    const LottieAnimation& a = *r.a;
    const LottieLayer& layer = a.layers.data[comp.layers + index];
    PrVec2 anchor = lottiePoint(a, a.transforms.data[layer.transform].anchor, frame, PrVec2{0, 0});
    PrMatrix4 chain = layerChain(r, comp, index, frame);
    PrMatrix4 m = prMatrix4Concat(&chain, &projection);
    float z = m.m[2][0] * anchor.x + m.m[2][1] * anchor.y + m.m[2][3];
    float w = m.m[3][0] * anchor.x + m.m[3][1] * anchor.y + m.m[3][3];
    return w > 0 ? z / w : -kPrF32Infinity;
}

void drawLayer(LottieRender& r, uint32_t compIndex, uint32_t index, double frame, const PrMatrix4* PR_NULLABLE camera);

// Returns the visible part of the root composition, in the space that `toCurrent` maps into the drawing space. It is
// the area that a mask or matte covers when it covers everything, as an inverted mask can.
PrRect visibleArea(const LottieRender& r, PrAffine2 toCurrent) {
    PrRect root{0, 0, r.a->info.extent.x, r.a->info.extent.y};
    PrAffine2 inverse = prAffine2Identity();
    if (!prAffine2Invert(prAffine2Concat(toCurrent, r.toRoot), &inverse))
        return root;
    return prAffine2MapRect(inverse, root);
}

// Returns a draw list of the matte layer as it plays. The layer that it mattes is masked by this list.
PrDrawList* PR_NULLABLE matteList(LottieRender& r, uint32_t compIndex, uint32_t index, double frame,
        const PrMatrix4* PR_NULLABLE camera) {
    PrDrawList* list = prDrawListCreate();
    PrDrawList* outer = r.list;
    r.list = list;
    drawLayer(r, compIndex, index, frame, camera);
    r.list = outer;
    if (prDrawListDepth(list) != 0) {
        prDrawListRelease(list);
        return nullptr;
    }
    return list;
}

// Writes the mask's path at `frame` into `path`, replacing its contents. Only a closed shape masks, so this returns
// false for an open one.
bool maskPath(LottieRender& r, const LottieMask& mask, double frame, PrPath* path) {
    const LottieAnimation& a = *r.a;
    prPathClear(path);
    uint32_t dims = lottieDims(a, mask.shape);
    if (dims < 2)
        return false;
    r.values.clear();
    r.values.reserve(dims);
    lottieEvaluate(a, mask.shape, frame, r.values.data);
    if (!lottieShapeClosed(r.values.data))
        return false;
    r.geometry.vertices.clear();
    r.geometry.contours.clear();
    lottieShapeFromValue(r.values.data, false, r.geometry);
    lottieAddContour(r.geometry, r.geometry.contours.data[0], prAffine2Identity(), path);
    return true;
}

// Draws one mask into the mask's list: its path grown or shrunk by its expansion, inverted against `area` if requested,
// and painted according to its mode.
void drawMask(LottieRender& r, const LottieMask& mask, double frame, PrRect area, PrDrawList* list) {
    const LottieAnimation& a = *r.a;
    float opacity = prF32Clamp(lottieScalar(a, mask.opacity, frame, 100) / 100, 0, 1);
    float expansion = lottieScalar(a, mask.expansion, frame, 0);
    PrPath* path = prPathCreate();
    if (!maskPath(r, mask, frame, path)) {
        // An open path masks nothing, so an intersection with it keeps nothing.
        prPathClear(path);
    }
    // A shrinking mask is the region that its path fills, less a band along its edge. The band is a polyline.
    PrPathPolyline shrunk{};
    if (expansion < 0) {
        PrPathPolyline flat{};
        prPathFlatten(path, prAffine2Identity(), kMaskTolerance, PrPathFlattening_Inside, &flat);
        prPathPolylineOffset(&flat, PrPathFillRule_NonZero, expansion, PrPathJoin_Miter, 4, kMaskTolerance, &shrunk);
        prPathPolylineReset(&flat);
    }
    PrPathFillRule rule = PrPathFillRule_NonZero;
    if (mask.inverted) {
        PrPath* outside = prPathCreate();
        prPathAddRect(outside, area);
        if (expansion < 0)
            prPathFlatten(outside, prAffine2Identity(), kMaskTolerance, PrPathFlattening_Inside, &shrunk);
        else
            prPathAddPath(path, outside, prAffine2Identity());
        prPathDestroy(outside);
        rule = PrPathFillRule_EvenOdd;
    }
    PrColor color = mask.mode == LottieMaskMode_Subtract ? PrColor{0, 0, 0, opacity} : PrColor{1, 1, 1, opacity};
    // An intersection keeps the earlier masks only where this mask covers, so it composites as a group that reaches
    // everywhere.
    bool intersect = mask.mode == LottieMaskMode_Intersect;
    if (intersect) {
        PrDrawGroup group = prDrawGroupDefault();
        group.composite = PrDrawComposite_DestinationIn;
        prDrawListPushGroup(list, &group);
    }
    if (expansion < 0) {
        prDrawListPolyline(list, &shrunk, rule, {.color = color});
    } else if (prPathVerbCount(path) > 0) {
        prDrawListPath(list, path, rule, {.color = color});
        if (expansion > 0 && !mask.inverted) {
            PrDrawStroke stroke = prDrawStrokeDefault();
            stroke.shape.width = expansion * 2;
            prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = color, .stroke = &stroke});
        }
    }
    if (intersect)
        prDrawListPopGroup(list);
    prPathPolylineReset(&shrunk);
    prPathDestroy(path);
}

// Pushes the result of the layer's masks for its content at `frame`, in the layer's space. One opaque additive path
// becomes a clip. Anything else becomes a luminance mask of the masks painted in turn, each by its mode. `area` is the
// visible area, in the layer's space. Returns the kind of push made: a clip, a group, or none.
enum {
    MaskPush_None,
    MaskPush_Clip,
    MaskPush_Group,
};
typedef uint8_t MaskPush;

MaskPush pushMasks(LottieRender& r, const LottieLayer& layer, double frame, PrRect area) {
    const LottieAnimation& a = *r.a;
    uint32_t used = 0;
    const LottieMask* only = nullptr;
    for (uint32_t i = 0; i < layer.maskCount; ++i) {
        const LottieMask& mask = a.masks.data[layer.masks + i];
        if (mask.mode == LottieMaskMode_None)
            continue;
        ++used;
        only = &mask;
    }
    if (used == 0)
        return MaskPush_None;
    if (used == 1 && only->mode != LottieMaskMode_Subtract && only->mode != LottieMaskMode_Intersect &&
            !only->inverted && lottieScalar(a, only->opacity, frame, 100) >= 100 &&
            lottieScalar(a, only->expansion, frame, 0) == 0) {
        PrPath* path = prPathCreate();
        maskPath(r, *only, frame, path);
        prDrawListPushClipPath(r.list, path, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
        prPathDestroy(path);
        return MaskPush_Clip;
    }
    PrDrawList* list = prDrawListCreate();
    bool first = true;
    for (uint32_t i = 0; i < layer.maskCount; ++i) {
        const LottieMask& mask = a.masks.data[layer.masks + i];
        if (mask.mode == LottieMaskMode_None)
            continue;
        // A first mask that subtracts or intersects starts from a mask that covers everything.
        if (first && (mask.mode == LottieMaskMode_Subtract || mask.mode == LottieMaskMode_Intersect))
            prDrawListRect(list, area, {.color = PrColor{1, 1, 1, 1}});
        first = false;
        drawMask(r, mask, frame, area, list);
    }
    PrDrawList* drawn = list;
    if (prDrawListDepth(list) != 0) {
        prDrawListRelease(list);
        drawn = nullptr;
    }
    PrDrawSource source = prDrawSourceList(drawn, area);
    PrDrawBrush brush = prDrawBrushImage(source, area, PrDrawSampling_Linear);
    PrDrawMask m = prDrawMaskDefault(&brush);
    m.mode = PrDrawMaskMode_Luminance;
    m.space = PrColorSpace_Srgb;
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = drawn != nullptr ? 1 : 0;
    group.mask = drawn != nullptr ? &m : nullptr;
    prDrawListPushGroup(r.list, &group);
    prDrawListRelease(drawn);
    return MaskPush_Group;
}

void drawContent(LottieRender& r, const LottieLayer& layer, double frame) {
    const LottieAnimation& a = *r.a;
    switch (layer.kind) {
        case LottieLayerKind_Shape:
            lottieDrawShapes(r, layer, frame);
            break;
        case LottieLayerKind_Text:
            lottieDrawText(r, layer, frame);
            break;
        case LottieLayerKind_Solid:
            prDrawListRect(r.list, PrRect{0, 0, layer.width, layer.height}, {.color = layer.color});
            break;
        case LottieLayerKind_Image: {
            if (layer.comp == kLottieNone)
                break;
            const LottieImage& image = a.images.data[layer.comp];
            if (image.image == nullptr)
                break;
            // An image whose size differs from its asset's size covers the asset's size, centered and clipped to it.
            float w = float(prDrawImageExtent(image.image).x);
            float h = float(prDrawImageExtent(image.image).y);
            float scale = prF32Max(image.width / w, image.height / h);
            float dx = (image.width - w * scale) / 2;
            float dy = (image.height - h * scale) / 2;
            PrDrawSource source = prDrawSourceImage(image.image, PrRect{0, 0, w, h});
            PrRect placed{dx, dy, dx + w * scale, dy + h * scale};
            PrDrawBrush brush = prDrawBrushImage(source, placed, PrDrawSampling_Linear);
            prDrawListPushClipRect(r.list, PrRect{0, 0, image.width, image.height}, PrDrawEdges_Antialiased);
            prDrawListRect(r.list, placed, {.brush = &brush});
            prDrawListPopClip(r.list);
            break;
        }
        case LottieLayerKind_Precomp: {
            // A composition that draws itself, directly or through other precomps, draws nothing at that point.
            if (layer.comp == kLottieNone || layer.comp == 0 || r.comps.count >= kLottieMaxDepth ||
                    r.comps.find(layer.comp) != kArrayMissing)
                break;
            double inner = (frame - layer.startTime) / layer.stretch;
            if (layer.timeRemap != kLottieNone) {
                inner = double(lottieScalar(a, layer.timeRemap, frame, 0)) * a.info.frameRate;
                // A remap onto the layer's out point draws the frame before it, so that a remap to the end holds the
                // last frame rather than drawing nothing.
                if (inner == layer.outPoint)
                    inner = layer.outPoint - 1;
            }
            r.comps.push(layer.comp);
            lottieDrawComp(r, layer.comp, inner, PrVec2{layer.width, layer.height});
            r.comps.pop();
            break;
        }
        default:
            break;
    }
}

// Draws a 3D layer under a camera: a projected group of the layer's plane, whose bounds are those of its content. The
// layer's opacity is the group's, and its clip, masks and effects stay in its plane.
void drawProjectedLayer(LottieRender& r, const LottieComp& comp, uint32_t index, double frame, float opacity,
        const PrMatrix4& camera) {
    const LottieAnimation& a = *r.a;
    const LottieLayer& layer = a.layers.data[comp.layers + index];
    PrMatrix4 chain = layerChain(r, comp, index, frame);
    PrMatrix4 projection = prMatrix4Concat(&chain, &camera);
    // The content first, so that its bounds are known.
    PrDrawList* content = prDrawListCreate();
    PrDrawList* outer = r.list;
    PrAffine2 outerRoot = r.toRoot;
    PrAffine2 flat = prAffine2Identity();
    prAffine2FromMatrix4(&chain, &flat);
    r.list = content;
    r.toRoot = prAffine2Concat(flat, r.toRoot);
    const PrDrawFilter* filter = lottieLayerFilter(r, layer, frame);
    if (filter != nullptr) {
        PrDrawGroup group = prDrawGroupDefault();
        group.filter = filter;
        prDrawListPushGroup(r.list, &group);
    }
    drawContent(r, layer, frame);
    if (filter != nullptr)
        prDrawListPopGroup(r.list);
    prDrawFilterRelease(filter);
    r.list = outer;
    r.toRoot = outerRoot;
    bool clipped = layer.kind == LottieLayerKind_Precomp;
    PrRect bounds = prDrawListBounds(content);
    // Content that covers its whole plane, as an unbounded operator does, covers the composition.
    if (!(prF32IsFinite(bounds.left) && prF32IsFinite(bounds.top) && prF32IsFinite(bounds.right) &&
            prF32IsFinite(bounds.bottom)))
        bounds = clipped ? PrRect{0, 0, layer.width, layer.height} : PrRect{0, 0, comp.width, comp.height};
    if (clipped)
        bounds = prRectIntersect(bounds, PrRect{0, 0, layer.width, layer.height});
    if (!prRectIsEmpty(bounds)) {
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = bounds;
        group.opacity = opacity;
        group.projection = &projection;
        prDrawListPushGroup(r.list, &group);
        if (clipped)
            prDrawListPushClipRect(r.list, PrRect{0, 0, layer.width, layer.height}, PrDrawEdges_Antialiased);
        MaskPush masked = layer.maskCount > 0 ? pushMasks(r, layer, frame, bounds) : MaskPush_None;
        prDrawListEmbed(r.list, content, PrDVec2{0, 0});
        if (masked == MaskPush_Clip)
            prDrawListPopClip(r.list);
        else if (masked == MaskPush_Group)
            prDrawListPopGroup(r.list);
        if (clipped)
            prDrawListPopClip(r.list);
        prDrawListPopGroup(r.list);
    }
    prDrawListRelease(content);
}

void drawLayer(LottieRender& r, uint32_t compIndex, uint32_t index, double frame, const PrMatrix4* PR_NULLABLE camera) {
    const LottieAnimation& a = *r.a;
    const LottieComp& comp = a.comps.data[compIndex];
    const LottieLayer& layer = a.layers.data[comp.layers + index];
    if (!inRange(layer, frame))
        return;
    const LottieTransform& transform = a.transforms.data[layer.transform];
    float opacity = lottieTransformOpacity(a, transform, frame);
    // A layer that is fully transparent draws nothing, whatever its effects and matte.
    if (!(opacity > 0))
        return;

    PrDrawMask mask{};
    PrDrawList* matte = nullptr;
    if (layer.matte != LottieMatte_None && layer.matteLayer != kLottieNone) {
        matte = matteList(r, compIndex, layer.matteLayer, frame, camera);
        PrRect area = visibleArea(r, prAffine2Identity());
        PrDrawSource source = prDrawSourceList(matte, area);
        PrDrawBrush brush = prDrawBrushImage(source, area, PrDrawSampling_Linear);
        mask = prDrawMaskDefault(&brush);
        bool luma = layer.matte == LottieMatte_Luma || layer.matte == LottieMatte_LumaInverted;
        mask.mode = luma ? PrDrawMaskMode_Luminance : PrDrawMaskMode_Alpha;
        mask.space = PrColorSpace_Srgb;
        mask.invert = layer.matte == LottieMatte_AlphaInverted || layer.matte == LottieMatte_LumaInverted;
    }
    bool blended = layer.blend != PrDrawBlendMode_Normal || layer.additive || layer.matte != LottieMatte_None;
    if (blended) {
        PrDrawGroup group = prDrawGroupDefault();
        group.blendMode = layer.blend;
        group.composite = layer.additive ? PrDrawComposite_PlusLighter : PrDrawComposite_SourceOver;
        group.mask = layer.matte != LottieMatte_None ? &mask : nullptr;
        // A matte that draws nothing hides the layer, or shows it whole if the matte is inverted.
        if (layer.matte != LottieMatte_None && matte == nullptr)
            group.opacity = mask.invert ? 1 : 0;
        if (matte == nullptr)
            group.mask = nullptr;
        prDrawListPushGroup(r.list, &group);
    }
    if (camera != nullptr && layer.threeD) {
        drawProjectedLayer(r, comp, index, frame, opacity, *camera);
        if (blended)
            prDrawListPopGroup(r.list);
        prDrawListRelease(matte);
        return;
    }
    PrAffine2 matrix = layerMatrix(r, comp, index, frame);
    prDrawListPushTransformAffine2(r.list, matrix);
    if (opacity < 1) {
        PrDrawGroup group = prDrawGroupDefault();
        group.opacity = opacity;
        prDrawListPushGroup(r.list, &group);
    }
    bool clipped = layer.kind == LottieLayerKind_Precomp;
    if (clipped)
        prDrawListPushClipRect(r.list, PrRect{0, 0, layer.width, layer.height}, PrDrawEdges_Antialiased);
    MaskPush masked = MaskPush_None;
    if (layer.maskCount > 0) {
        // The visible area in the layer's space, which is where the masks' fills start from.
        masked = pushMasks(r, layer, frame, visibleArea(r, matrix));
    }
    // Effects filter the content before the masks and clips cut it, which is the order that SVG uses.
    const PrDrawFilter* filter = lottieLayerFilter(r, layer, frame);
    if (filter != nullptr) {
        PrDrawGroup group = prDrawGroupDefault();
        group.filter = filter;
        prDrawListPushGroup(r.list, &group);
    }
    PrAffine2 outer = r.toRoot;
    r.toRoot = prAffine2Concat(matrix, r.toRoot);
    drawContent(r, layer, frame);
    r.toRoot = outer;
    if (filter != nullptr)
        prDrawListPopGroup(r.list);
    prDrawFilterRelease(filter);
    if (masked == MaskPush_Clip)
        prDrawListPopClip(r.list);
    else if (masked == MaskPush_Group)
        prDrawListPopGroup(r.list);
    if (clipped)
        prDrawListPopClip(r.list);
    if (opacity < 1)
        prDrawListPopGroup(r.list);
    prDrawListPopTransform(r.list);
    if (blended)
        prDrawListPopGroup(r.list);
    prDrawListRelease(matte);
}

} // namespace

void lottieRenderInit(LottieRender& r, const LottieAnimation& a, PrDrawList* list) {
    r = LottieRender{};
    r.a = &a;
    r.list = list;
    r.toRoot = prAffine2Identity();
    r.path = prPathCreate();
}

void lottieRenderReset(LottieRender& r) {
    for (uint32_t i = 0; i < r.paths.count; ++i)
        prPathDestroy(r.paths.data[i]);
    r.paths.reset();
    prPathDestroy(r.path);
    r.geometry.vertices.reset();
    r.geometry.contours.reset();
    r.groups.reset();
    r.shapes.reset();
    r.styles.reset();
    r.modifiers.reset();
    r.steps.reset();
    r.refs.reset();
    r.values.reset();
    r.stops.reset();
    r.dashes.reset();
    r.comps.reset();
}

void lottieDrawComp(LottieRender& r, uint32_t comp, double frame, PrVec2 extent) {
    const LottieAnimation& a = *r.a;
    const LottieComp& c = a.comps.data[comp];
    PrMatrix4 projection;
    const PrMatrix4* camera = cameraProjection(r, c, frame, extent, &projection) ? &projection : nullptr;
    // The layers to draw, bottom first. Under a camera, each run of consecutive 3D layers is sorted by the depth of
    // each anchor, farthest first, so that nearer planes draw over farther ones. Planes that intersect are not split.
    RawArray<uint32_t> order;
    RawArray<float> depths;
    for (uint32_t i = c.count; i-- > 0;) {
        const LottieLayer& layer = a.layers.data[c.layers + i];
        if (layer.hidden || layer.matteSource || layer.kind == LottieLayerKind_Camera)
            continue;
        order.push(i);
        depths.push(camera != nullptr && layer.threeD ? cameraDepth(r, c, i, frame, *camera) : 0);
    }
    for (uint32_t start = 0; camera != nullptr && start < order.count;) {
        uint32_t end = start;
        while (end < order.count && a.layers.data[c.layers + order.data[end]].threeD)
            ++end;
        // Insertion sort keeps layers of equal depth in their order.
        for (uint32_t i = start + 1; i < end; ++i) {
            for (uint32_t k = i; k > start && depths.data[k - 1] > depths.data[k]; --k) {
                float depth = depths.data[k];
                depths.data[k] = depths.data[k - 1];
                depths.data[k - 1] = depth;
                uint32_t layer = order.data[k];
                order.data[k] = order.data[k - 1];
                order.data[k - 1] = layer;
            }
        }
        start = end == start ? start + 1 : end;
    }
    for (uint32_t i = 0; i < order.count; ++i) {
        if (r.comps.count != 0 && r.instances++ >= kLottieMaxInstances)
            break;
        drawLayer(r, comp, order.data[i], frame, camera);
    }
    order.reset();
    depths.reset();
}

} // namespace pr_impl
