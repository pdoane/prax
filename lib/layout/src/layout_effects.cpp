// Effects: the shapes, masks and filter lists that a tree interns for its compositing groups, and how a box's clip
// path, mask, filters and blending apply around its content.
//
// css-masking-1 - https://drafts.fxtf.org/css-masking-1/
// css-shapes-1 - https://drafts.csswg.org/css-shapes-1/
// filter-effects-1 - https://drafts.fxtf.org/filter-effects-1/
// filter-effects-2 - https://drafts.fxtf.org/filter-effects-2/

#include "layout/src/layout_effects.h"

#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_fragment.h"
#include "layout/src/layout_paint.h"
#include "layout/src/layout_stacking.h"
#include "layout/src/layout_style.h"
#include "layout/src/layout_tree.h"

namespace pr_impl {

namespace {

// The seed for hashing an entry's key.
constexpr uint64_t kEffectSeed = 0x510e527fade682d1ull;

// The luminance coefficients of filter-effects-1's matrices (filter-effects-1/#feColorMatrixElement).
constexpr float kRed = 0.2126f;
constexpr float kGreen = 0.7152f;
constexpr float kBlue = 0.0722f;

// How far a rectangle that stands for everything reaches, in pixels: the backdrop that a backdrop filter erases outside
// its box.
constexpr float kEverywhere = float(1 << 24);

template <typename T>
void appendKey(Array<uint8_t>& key, const T& value) {
    key.append(reinterpret_cast<const uint8_t*>(&value), uint32_t(sizeof(T)));
}

void countLength(LayoutCompounds* compounds, PrLayoutLength length, uint32_t delta) {
    PR_ASSERT(prLayoutWordKind(length.bits) != PrLayoutWordKind_Number
              && prLayoutWordKind(length.bits) != PrLayoutWordKind_Integer);
    layoutCompoundsCount(compounds, length.bits, delta);
}

// The lengths that a shape names.
template <typename Each>
void eachShapeLength(const PrLayoutShape& shape, Each each) {
    const PrLayoutLength lengths[] = {shape.radiusX, shape.radiusY, shape.centerX, shape.centerY};
    for (PrLayoutLength length : lengths)
        each(length);
    for (PrLayoutLength length : shape.inset)
        each(length);
    for (PrLayoutLength length : shape.radii)
        each(length);
    for (uint32_t i = 0; i < 2 * shape.pointCount; ++i)
        each(shape.points[i]);
}

// The lengths that a mask border names.
template <typename Each>
void eachBorderLength(const PrLayoutMaskBorder& border, Each each) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        each(border.slice[side]);
        each(border.width[side]);
        each(border.outset[side]);
    }
}

void shapeKey(Array<uint8_t>& key, const PrLayoutShape& shape) {
    uint8_t bytes[] = {shape.kind, shape.rule, shape.extentX, shape.extentY};
    appendKey(key, bytes);
    eachShapeLength(shape, [&](PrLayoutLength length) { appendKey(key, length.bits); });
    appendKey(key, shape.pointCount);
    appendKey(key, shape.round);
    if (shape.path != nullptr) {
        uint32_t verbs = prPathVerbCount(shape.path);
        uint32_t points = prPathPointCount(shape.path);
        appendKey(key, verbs);
        appendKey(key, points);
        if (verbs != 0)
            key.append(reinterpret_cast<const uint8_t*>(prPathVerbs(shape.path)), verbs * uint32_t(sizeof(PrPathVerb)));
        if (points != 0)
            key.append(reinterpret_cast<const uint8_t*>(prPathPoints(shape.path)), points * uint32_t(sizeof(PrVec2)));
    }
}

void maskKey(Array<uint8_t>& key, const PrLayoutMask& mask) {
    appendKey(key, mask.layerCount);
    for (uint32_t i = 0; i < mask.layerCount; ++i) {
        const PrLayoutMaskLayer& layer = mask.layers[i];
        layoutBackgroundKey(key, &layer.image, 1);
        layoutSourceKey(key, layer.source);
        uint8_t bytes[] = {layer.mode, layer.composite, uint8_t(layer.unclipped)};
        appendKey(key, bytes);
    }
    const PrLayoutMaskBorder& border = mask.border;
    layoutBackgroundKey(key, &border.image, 1);
    eachBorderLength(border, [&](PrLayoutLength length) { appendKey(key, length.bits); });
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        uint8_t kinds[] = {border.widthKind[side], border.outsetKind[side]};
        appendKey(key, kinds);
        appendKey(key, border.widthNumber[side]);
        appendKey(key, border.outsetNumber[side]);
    }
    uint8_t bytes[] = {uint8_t(border.fill), border.horizontal, border.vertical, border.mode};
    appendKey(key, bytes);
}

// Adds `delta` to the references of what an entry of `kind` holds: its compounds, images, lists, painters and sources.
void countEntry(LayoutCompounds* compounds, LayoutEffectKind kind, const LayoutEffectEntry& entry, uint32_t delta) {
    switch (kind) {
        case LayoutEffectKind_Shape:
            eachShapeLength(entry.shape, [&](PrLayoutLength length) { countLength(compounds, length, delta); });
            break;
        case LayoutEffectKind_Mask:
            for (uint32_t i = 0; i < entry.mask.layerCount; ++i) {
                layoutBackgroundCountLayers(compounds, &entry.mask.layers[i].image, 1, delta);
                layoutSourceCount(entry.mask.layers[i].source, delta);
            }
            layoutBackgroundCountLayers(compounds, &entry.mask.border.image, 1, delta);
            eachBorderLength(entry.mask.border, [&](PrLayoutLength length) { countLength(compounds, length, delta); });
            break;
        default:
            for (uint32_t i = 0; i < entry.filterCount; ++i)
                layoutSourceCount(entry.filters[i].source, delta);
            break;
    }
}

// Copies the gradient stops of `image` into `stops` from `next`, and points the image at the copy.
void takeStops(PrLayoutBackgroundLayer& image, PrLayoutColorStop* PR_NULLABLE stops, uint32_t* next) {
    if (image.kind != PrLayoutImageKind_Gradient || image.gradient.stopCount == 0)
        return;
    memCopy(stops + *next, image.gradient.stops, sizeof(PrLayoutColorStop) * image.gradient.stopCount);
    image.gradient.stops = stops + *next;
    *next += image.gradient.stopCount;
}

uint32_t stopsOf(const PrLayoutBackgroundLayer& image) {
    return image.kind == PrLayoutImageKind_Gradient ? image.gradient.stopCount : 0;
}

// Fills `entry` with a copy of the value that its kind holds, which it owns.
void copyInto(LayoutEffectEntry& entry, LayoutEffectKind kind, const PrLayoutShape* shape, const PrLayoutMask* mask,
              const PrLayoutFilter* filters, uint32_t filterCount) {
    switch (kind) {
        case LayoutEffectKind_Shape: {
            entry.shape = *shape;
            entry.shape.points = nullptr;
            entry.shape.path = nullptr;
            if (shape->pointCount != 0) {
                size_t bytes = sizeof(PrLayoutLength) * 2 * shape->pointCount;
                PrLayoutLength* points = static_cast<PrLayoutLength*>(prMemoryAlloc(bytes));
                memCopy(points, shape->points, bytes);
                entry.shape.points = points;
            }
            if (shape->path != nullptr)
                entry.shape.path = prPathClone(shape->path);
            break;
        }
        case LayoutEffectKind_Mask: {
            entry.mask = *mask;
            entry.mask.layers = nullptr;
            uint32_t stopCount = stopsOf(mask->border.image);
            uint32_t imageFilters = mask->border.image.filterCount;
            for (uint32_t i = 0; i < mask->layerCount; ++i) {
                stopCount += stopsOf(mask->layers[i].image);
                imageFilters += mask->layers[i].image.filterCount;
            }
            entry.stops = stopCount != 0
                              ? static_cast<PrLayoutColorStop*>(prMemoryAlloc(sizeof(PrLayoutColorStop) * stopCount))
                              : nullptr;
            entry.filters = imageFilters != 0
                                ? static_cast<PrLayoutFilter*>(prMemoryAlloc(sizeof(PrLayoutFilter) * imageFilters))
                                : nullptr;
            uint32_t next = 0;
            uint32_t nextFilter = 0;
            if (mask->layerCount != 0) {
                PrLayoutMaskLayer* layers =
                    static_cast<PrLayoutMaskLayer*>(prMemoryAlloc(sizeof(PrLayoutMaskLayer) * mask->layerCount));
                memCopy(layers, mask->layers, sizeof(PrLayoutMaskLayer) * mask->layerCount);
                for (uint32_t i = 0; i < mask->layerCount; ++i) {
                    takeStops(layers[i].image, entry.stops, &next);
                    layoutImageTakeFilters(layers[i].image, entry.filters, &nextFilter);
                }
                entry.mask.layers = layers;
            }
            takeStops(entry.mask.border.image, entry.stops, &next);
            layoutImageTakeFilters(entry.mask.border.image, entry.filters, &nextFilter);
            break;
        }
        default:
            entry.filters = static_cast<PrLayoutFilter*>(prMemoryAlloc(sizeof(PrLayoutFilter) * filterCount));
            memCopy(entry.filters, filters, sizeof(PrLayoutFilter) * filterCount);
            entry.filterCount = filterCount;
            break;
    }
}

// Releases the image, list or painter that an image holds, and the sources of its filter list, apart from its
// compounds.
void releaseImage(const PrLayoutBackgroundLayer& image) {
    if (image.kind == PrLayoutImageKind_Image)
        prDrawImageRelease(image.image);
    else if (image.kind == PrLayoutImageKind_List)
        prDrawListRelease(image.list);
    else if (image.kind == PrLayoutImageKind_Painter)
        prLayoutPainterRelease(image.painter);
    for (uint32_t i = 0; i < image.filterCount; ++i)
        layoutSourceCount(image.filters[i].source, 0xffffffffu);
}

void freeEntry(LayoutEffectEntry& entry) {
    prMemoryFree(const_cast<PrLayoutLength*>(entry.shape.points));
    prPathDestroy(const_cast<PrPath*>(entry.shape.path));
    prMemoryFree(const_cast<PrLayoutMaskLayer*>(entry.mask.layers));
    prMemoryFree(entry.filters);
    prMemoryFree(entry.stops);
    entry.key.reset();
    entry = LayoutEffectEntry{};
}

// Interns the value of `kind` whose key is `key`, which the entry takes.
uint32_t intern(LayoutEffectLists* lists, LayoutCompounds* compounds, LayoutEffectKind kind, Array<uint8_t>& key,
                const PrLayoutShape* shape, const PrLayoutMask* mask, const PrLayoutFilter* filters,
                uint32_t filterCount) {
    LayoutEffectTable& table = lists->tables[kind];
    uint32_t hash = uint32_t(hashBytes(key.data, key.count, kEffectSeed));
    uint32_t found = table.index.find(hash, [&](uint32_t i) {
        const LayoutEffectEntry& e = table.entries.data[i];
        return e.key.count == key.count && memEquals(e.key.data, key.data, key.count);
    });
    if (found != kHashMissing) {
        key.reset();
        return found;
    }
    uint32_t at = 0;
    if (table.free.count != 0) {
        at = table.free.last();
        table.free.pop();
    } else {
        at = table.entries.count;
        table.entries.push(LayoutEffectEntry{});
    }
    LayoutEffectEntry& entry = table.entries.data[at];
    copyInto(entry, kind, shape, mask, filters, filterCount);
    entry.live = true;
    entry.key = key;
    entry.hash = hash;
    entry.references = 0;
    countEntry(compounds, kind, entry, 1);
    table.index.insert(hash, at);
    table.candidates.push(at);
    return at;
}

void countAt(LayoutEffectTable& table, uint32_t index, uint32_t delta) {
    if (index == 0)
        return;
    LayoutEffectEntry& e = table.entries.data[index];
    PR_ASSERT(e.live && (delta == 1 || e.references != 0));
    e.references += delta;
    if (e.references == 0)
        table.candidates.push(index);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

float resolvePx(const LayoutCompounds* PR_NULLABLE compounds, PrLayoutLength length, float base) {
    return prLayoutUnitToPx(layoutLengthResolve(compounds, length, prLayoutUnitFromPx(base)));
}

// Returns the radius that an extent gives along one axis from a center that lies at `near` and `far` from the axis's
// two sides (css-images-3/#typedef-radial-extent). A corner extent takes the side's distance scaled as an ellipse
// through the corner would be, for an axis taken alone.
float extentOf(PrLayoutRadialExtent extent, float near, float far) {
    switch (extent) {
        case PrLayoutRadialExtent_FarthestSide: return maxF32(near, far);
        case PrLayoutRadialExtent_ClosestCorner: return minF32(near, far) * kSqrt2F32;
        case PrLayoutRadialExtent_FarthestCorner: return maxF32(near, far) * kSqrt2F32;
        default: return minF32(near, far);
    }
}

// Returns the radii of corners whose lengths are `radii`, as PrLayoutShape::radii orders them, with percentages of
// `extent`.
void cornersOf(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutLength* radii, PrVec2 extent,
               PrCorner* corners) {
    for (uint32_t c = 0; c < PrRectCorner_Count; ++c) {
        PrVec2 r{maxF32(resolvePx(compounds, radii[2 * c], extent.x), 0),
                 maxF32(resolvePx(compounds, radii[2 * c + 1], extent.y), 0)};
        corners[c] = prCornerRound(r);
    }
}

void insetPath(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutShape& shape, PrRect box, PrPath* out) {
    PrVec2 extent{prRectWidth(box), prRectHeight(box)};
    float insets[PrRectSide_Count];
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        insets[side] = resolvePx(compounds, shape.inset[side], side % 2 == 0 ? extent.y : extent.x);
    for (uint32_t axis = 0; axis < 2; ++axis) {
        float& near = insets[axis == 0 ? PrRectSide_Top : PrRectSide_Left];
        float& far = insets[axis == 0 ? PrRectSide_Bottom : PrRectSide_Right];
        float whole = axis == 0 ? extent.y : extent.x;
        float sum = near + far;
        if (sum > whole && sum > 0) {
            near *= whole / sum;
            far *= whole / sum;
        }
    }
    PrRect rect{box.left + insets[PrRectSide_Left], box.top + insets[PrRectSide_Top],
                box.right - insets[PrRectSide_Right], box.bottom - insets[PrRectSide_Bottom]};
    if (!(prRectWidth(rect) > 0) || !(prRectHeight(rect) > 0))
        return;
    PrCornerRect corners{.rect = rect};
    // A corner's percentages are of the reference box (css-shapes-1/#funcdef-basic-shape-inset).
    cornersOf(compounds, shape.radii, extent, corners.corners);
    prPathAddCornerRect(out, prCornerRectFit(corners));
}

void ellipsePath(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutShape& shape, PrRect box, PrPath* out) {
    PrVec2 extent{prRectWidth(box), prRectHeight(box)};
    PrVec2 center{box.left + resolvePx(compounds, shape.centerX, extent.x),
                  box.top + resolvePx(compounds, shape.centerY, extent.y)};
    PrVec2 near{center.x - box.left, center.y - box.top};
    PrVec2 far{box.right - center.x, box.bottom - center.y};
    PrVec2 radii{};
    if (shape.kind == PrLayoutShapeKind_Circle) {
        PrLayoutRadialExtent e = shape.extentX;
        if (e == PrLayoutRadialExtent_Radii) {
            float diagonal = sqrtF32((extent.x * extent.x + extent.y * extent.y) / 2);
            radii.x = resolvePx(compounds, shape.radiusX, diagonal);
        } else if (e == PrLayoutRadialExtent_ClosestCorner || e == PrLayoutRadialExtent_FarthestCorner) {
            // The distance to the nearest or farthest corner.
            bool closest = e == PrLayoutRadialExtent_ClosestCorner;
            float dx = closest ? minF32(near.x, far.x) : maxF32(near.x, far.x);
            float dy = closest ? minF32(near.y, far.y) : maxF32(near.y, far.y);
            radii.x = sqrtF32(dx * dx + dy * dy);
        } else {
            float x = extentOf(e, near.x, far.x);
            float y = extentOf(e, near.y, far.y);
            radii.x = e == PrLayoutRadialExtent_FarthestSide ? maxF32(x, y) : minF32(x, y);
        }
        radii.y = radii.x;
    } else {
        radii.x = shape.extentX == PrLayoutRadialExtent_Radii ? resolvePx(compounds, shape.radiusX, extent.x)
                                                              : extentOf(shape.extentX, near.x, far.x);
        radii.y = shape.extentY == PrLayoutRadialExtent_Radii ? resolvePx(compounds, shape.radiusY, extent.y)
                                                              : extentOf(shape.extentY, near.y, far.y);
    }
    if (radii.x > 0 && radii.y > 0)
        prPathAddEllipse(out, center, radii, 0);
}

// Appends a polygon's vertices, each rounded by `round` if it is above zero
// (css-shapes-1/#funcdef-basic-shape-polygon): by an arc of a circle of that radius, tangent to both sides of the
// vertex, whose radius is clamped so that it rounds no more than half of either side.
void polygonPath(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutShape& shape, PrRect box, PrPath* out) {
    PrVec2 extent{prRectWidth(box), prRectHeight(box)};
    uint32_t count = shape.pointCount;
    if (count == 0)
        return;
    Array<PrVec2> points;
    for (uint32_t i = 0; i < count; ++i)
        points.push(PrVec2{box.left + resolvePx(compounds, shape.points[2 * i], extent.x),
                           box.top + resolvePx(compounds, shape.points[2 * i + 1], extent.y)});
    if (!(shape.round > 0) || count < 3) {
        prPathMoveTo(out, points.data[0]);
        for (uint32_t i = 1; i < count; ++i)
            prPathLineTo(out, points.data[i]);
        prPathClose(out);
        points.reset();
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 at = points.data[i];
        PrVec2 before = points.data[(i + count - 1) % count];
        PrVec2 after = points.data[(i + 1) % count];
        PrVec2 a{before.x - at.x, before.y - at.y};
        PrVec2 b{after.x - at.x, after.y - at.y};
        float la = sqrtF32(a.x * a.x + a.y * a.y);
        float lb = sqrtF32(b.x * b.x + b.y * b.y);
        float cosine = la > 0 && lb > 0 ? clampF32((a.x * b.x + a.y * b.y) / (la * lb), -1, 1) : 1;
        float angle = acosF32(cosine);
        float half = tanF32(angle / 2);
        // A vertex at which the sides meet straight or fold back is not rounded.
        if (!(la > 0) || !(lb > 0) || !(half > 0) || !isFiniteF32(half)) {
            if (i == 0)
                prPathMoveTo(out, at);
            else
                prPathLineTo(out, at);
            continue;
        }
        float radius = minF32(shape.round, minF32(half * la / 2, half * lb / 2));
        float distance = radius / half;
        PrVec2 in{at.x + a.x / la * distance, at.y + a.y / la * distance};
        PrVec2 outward{at.x + b.x / lb * distance, at.y + b.y / lb * distance};
        // The cubic that approximates the arc: its handles lie along the sides, toward the vertex.
        float sweep = kPiF32 - angle;
        float handle = 4.0f / 3.0f * tanF32(sweep / 4) * radius / distance;
        PrVec2 c1{in.x + (at.x - in.x) * handle, in.y + (at.y - in.y) * handle};
        PrVec2 c2{outward.x + (at.x - outward.x) * handle, outward.y + (at.y - outward.y) * handle};
        if (i == 0)
            prPathMoveTo(out, in);
        else
            prPathLineTo(out, in);
        prPathCubicTo(out, c1, c2, outward);
    }
    prPathClose(out);
    points.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Filters

// Returns the matrix that mixes `full`, the matrix of the whole effect, with the identity by `amount`
// (filter-effects-1/#grayscaleEquivalent, filter-effects-1/#sepiaEquivalent).
PrAffine4 mixedMatrix(const float full[3][3], float amount) {
    PrAffine4 m{};
    for (uint32_t r = 0; r < 3; ++r) {
        for (uint32_t c = 0; c < 3; ++c)
            m.m[r][c] = full[r][c] * amount + (r == c ? 1 - amount : 0);
    }
    m.m[3][3] = 1;
    return m;
}

// Appends a node of `kind` that reads `input`, set up by `fill`, and returns it.
template <typename Fill>
PrDrawFilterInput addNode(PrDrawFilterBuilder* builder, PrDrawFilterKind kind, PrDrawFilterInput input, Fill fill) {
    PrDrawFilterNode node{};
    node.kind = kind;
    node.space = PrColorSpace_Srgb;
    node.inputs[0] = input;
    fill(node);
    return prDrawFilterBuilderAdd(builder, &node);
}

// Returns a transfer function of a straight line.
PrDrawTransfer linear(float slope, float intercept) {
    return PrDrawTransfer{.kind = PrDrawTransferKind_Linear, .slope = slope, .intercept = intercept};
}

PrDrawFilterInput appendFunction(const PrLayoutFilter& f, PrDrawFilterInput input, PrImageEdgeMode edge,
                                 PrDrawFilterBuilder* builder) {
    float a = f.amount;
    switch (f.kind) {
        case PrLayoutFilterKind_Blur:
            return addNode(builder, PrDrawFilterKind_Blur, input, [&](PrDrawFilterNode& n) {
                n.blur = PrDrawFilterBlur{.sigma = PrVec2{a, a}, .edge = edge};
            });
        case PrLayoutFilterKind_DropShadow:
            return addNode(builder, PrDrawFilterKind_DropShadow, input, [&](PrDrawFilterNode& n) {
                n.shadow = PrDrawFilterShadow{.offset = f.offset, .sigma = PrVec2{f.sigma, f.sigma}, .color = f.color};
            });
        case PrLayoutFilterKind_Brightness:
        case PrLayoutFilterKind_Contrast:
        case PrLayoutFilterKind_Invert:
        case PrLayoutFilterKind_Opacity:
            return addNode(builder, PrDrawFilterKind_ComponentTransfer, input, [&](PrDrawFilterNode& n) {
                // The tables of invert() and opacity() (filter-effects-1/#invertEquivalent), as the lines through their
                // two values.
                PrDrawTransfer color{.kind = PrDrawTransferKind_Identity};
                PrDrawTransfer alpha{.kind = PrDrawTransferKind_Identity};
                if (f.kind == PrLayoutFilterKind_Brightness)
                    color = linear(a, 0);
                else if (f.kind == PrLayoutFilterKind_Contrast)
                    color = linear(a, 0.5f - 0.5f * a);
                else if (f.kind == PrLayoutFilterKind_Invert)
                    color = linear(1 - 2 * a, a);
                else
                    alpha = linear(a, 0);
                n.transfer[PrDrawChannel_R] = color;
                n.transfer[PrDrawChannel_G] = color;
                n.transfer[PrDrawChannel_B] = color;
                n.transfer[PrDrawChannel_A] = alpha;
            });
        case PrLayoutFilterKind_Grayscale:
        case PrLayoutFilterKind_Sepia:
        case PrLayoutFilterKind_Saturate:
        case PrLayoutFilterKind_HueRotate:
            return addNode(builder, PrDrawFilterKind_ColorMatrix, input, [&](PrDrawFilterNode& n) {
                static const float kGray[3][3] = {
                    {kRed, kGreen, kBlue}, {kRed, kGreen, kBlue}, {kRed, kGreen, kBlue}};
                static const float kSepia[3][3] = {
                    {0.393f, 0.769f, 0.189f}, {0.349f, 0.686f, 0.168f}, {0.272f, 0.534f, 0.131f}};
                if (f.kind == PrLayoutFilterKind_Grayscale)
                    n.matrix = mixedMatrix(kGray, a);
                else if (f.kind == PrLayoutFilterKind_Sepia)
                    n.matrix = mixedMatrix(kSepia, a);
                else if (f.kind == PrLayoutFilterKind_Saturate)
                    n.matrix = prDrawColorMatrixSaturate(a);
                else
                    n.matrix = prDrawColorMatrixHueRotate(a);
            });
        default:
            return input;
    }
}

// Returns the filter of the list at `index`, for a box whose border box is `box` and whose source filters' object
// bounding box is `bounds` (PrLayoutSourceFunctions::filter), or null for none. Writes the region of a source that ends
// the list to `region`. Sets `hidden` if a source hides the box. A backdrop's blur reads past its edges by mirroring
// (filter-effects-2/#backdrop-filter-operation), within `edges`.
const PrDrawFilter* PR_NULLABLE filterOf(const LayoutEffectLists* lists, PrLayoutFilterIndex index, PrRect box,
                                         PrRect bounds, bool backdrop, PrRect edges, PrRect* region, bool* hidden) {
    *region = PrRect{};
    if (index == kPrLayoutFilterNone)
        return nullptr;
    const LayoutEffectEntry& entry = layoutEffectAt(lists, LayoutEffectKind_Filter, index);
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    if (backdrop)
        prDrawFilterBuilderSetRegion(builder, edges);
    PrDrawFilterInput at = backdrop ? PrDrawFilterInput_Backdrop : PrDrawFilterInput_Source;
    uint32_t added = 0;
    for (uint32_t i = 0; i < entry.filterCount && !*hidden; ++i) {
        const PrLayoutFilter& f = entry.filters[i];
        *region = PrRect{};
        if (f.kind == PrLayoutFilterKind_Source) {
            const PrLayoutSourceFunctions* functions = f.source.functions;
            PrLayoutSourceFilter result{.result = at};
            if (functions == nullptr || functions->filter == nullptr)
                continue;
            if (!functions->filter(f.source.context, box, bounds, at, builder, &result)) {
                *hidden = true;
                break;
            }
            if (result.result != at)
                ++added;
            at = result.result;
            *region = result.region;
            continue;
        }
        at = appendFunction(f, at, backdrop ? PrImageEdgeMode_Mirror : PrImageEdgeMode_None, builder);
        ++added;
    }
    // A list whose items added nothing still filters, as an identity, so that its box keeps its group.
    if (!*hidden && added == 0)
        at = addNode(builder, PrDrawFilterKind_Offset, at, [](PrDrawFilterNode& n) { n.offset = PrVec2{0, 0}; });
    const PrDrawFilter* filter = *hidden ? nullptr : prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

// ---------------------------------------------------------------------------------------------------------------------
// Masks

// A mask layer as drawn: the list whose values mask over its area, and how its values are read. A transparent layer has
// no list.
struct DrawnLayer {
    PrDrawList* PR_NULLABLE list;
    PrRect area;
    PrDrawMaskMode mode;
    PrColorSpace space;
};

// Returns the operator that composites a layer of `composite` onto the layers below it: the operator itself, as
// css-masking-1/#the-mask-composite maps add, subtract, intersect and exclude.
PrDrawComposite layerOperator(PrDrawComposite composite) {
    return composite;
}

// Returns the natural size of an image, or `fallback` in an axis in which it has none.
PrVec2 naturalOf(const PrLayoutBackgroundLayer& image, PrVec2 fallback) {
    if (image.kind == PrLayoutImageKind_Image) {
        PrUVec2 texels = prDrawImageExtent(image.image);
        return PrVec2{float(texels.x), float(texels.y)};
    }
    if (image.kind == PrLayoutImageKind_Gradient)
        return fallback;
    return PrVec2{image.natural.width.raw >= 0 ? prLayoutUnitToPx(image.natural.width) : fallback.x,
                  image.natural.height.raw >= 0 ? prLayoutUnitToPx(image.natural.height) : fallback.y};
}

// Draws a mask border into a list of its own (css-masking-1/#masking-with-the-mask-border-image): the nine pieces of
// its image placed in the border image area, the border box grown by the outsets, as border-image places them
// (css-backgrounds-3/#border-image-process). The middle masks nothing unless the slices fill it. `border` is the box's
// border box and `widths` its border's widths.
bool drawBorder(const LayoutCompounds* compounds, const PrLayoutMaskBorder& b, PrRect border, const float* widths,
                DrawnLayer* out) {
    out->mode = b.mode;
    out->space = PrColorSpace_Srgb;
    auto edgeOf = [&](PrLayoutBorderImageKind kind, PrLayoutLength length, float number, float base,
                      uint32_t side) -> float {
        if (kind == PrLayoutBorderImageKind_Number)
            return number * widths[side];
        return maxF32(resolvePx(compounds, length, base), 0);
    };
    PrVec2 borderExtent{prRectWidth(border), prRectHeight(border)};
    float outsets[PrRectSide_Count];
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        outsets[side] = edgeOf(b.outsetKind[side], b.outset[side], b.outsetNumber[side],
                               side % 2 == 0 ? borderExtent.y : borderExtent.x, side);
    PrRect area{border.left - outsets[PrRectSide_Left], border.top - outsets[PrRectSide_Top],
                border.right + outsets[PrRectSide_Right], border.bottom + outsets[PrRectSide_Bottom]};
    PrVec2 extent{prRectWidth(area), prRectHeight(area)};
    out->area = area;
    if (prRectIsEmpty(area))
        return false;
    // An image without natural dimensions takes the border image area's (css-backgrounds-3/#border-image-source).
    PrVec2 natural = naturalOf(b.image, extent);
    PrDrawNineSlice slices{};
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        float whole = side % 2 == 0 ? natural.y : natural.x;
        slices.slices[side] = clampF32(resolvePx(compounds, b.slice[side], whole), 0, whole);
        float base = side % 2 == 0 ? extent.y : extent.x;
        slices.widths[side] = b.widthKind[side] == PrLayoutBorderImageKind_Auto
                                  ? slices.slices[side]
                                  : edgeOf(b.widthKind[side], b.width[side], b.widthNumber[side], base, side);
    }
    // Widths that overlap are reduced in proportion (css-backgrounds-3/#border-image-width).
    float factor = 1;
    if (slices.widths[PrRectSide_Left] + slices.widths[PrRectSide_Right] > extent.x)
        factor = minF32(factor, extent.x / (slices.widths[PrRectSide_Left] + slices.widths[PrRectSide_Right]));
    if (slices.widths[PrRectSide_Top] + slices.widths[PrRectSide_Bottom] > extent.y)
        factor = minF32(factor, extent.y / (slices.widths[PrRectSide_Top] + slices.widths[PrRectSide_Bottom]));
    for (float& width : slices.widths)
        width *= factor;
    slices.horizontal = b.horizontal;
    slices.vertical = b.vertical;
    slices.center = b.fill;
    PrDrawList* drawn = nullptr;
    PrDrawSource source = layoutImageSource(compounds, b.image, natural, &drawn);
    out->list = prDrawListCreate();
    prDrawListNineSlice(out->list, source, &slices, area, PrDrawSampling_Linear,
                        PrDrawPaint{.color = PR_COLOR16(1, 1, 1, 1)});
    if (!b.fill) {
        // The middle is opaque white, which masks nothing.
        PrRect middle{area.left + slices.widths[PrRectSide_Left], area.top + slices.widths[PrRectSide_Top],
                      area.right - slices.widths[PrRectSide_Right], area.bottom - slices.widths[PrRectSide_Bottom]};
        if (!prRectIsEmpty(middle))
            prDrawListRect(out->list, middle, PrDrawPaint{.color = PR_COLOR16(1, 1, 1, 1)});
    }
    prDrawListRelease(drawn);
    return true;
}

// Draws a mask layer of an image into a list of its own, placed as a background layer in `boxes`. A layer that is not
// clipped paints over `ink`.
void drawImageLayer(const LayoutCompounds* compounds, const PrLayoutMaskLayer& layer, LayoutBackgroundBoxes boxes,
                    PrRect ink, DrawnLayer* out) {
    out->mode = layer.mode == PrLayoutMaskMode_Luminance ? PrDrawMaskMode_Luminance : PrDrawMaskMode_Alpha;
    out->space = PrColorSpace_Srgb;
    boxes.area = layer.unclipped ? ink : PrRect{};
    out->area = layer.unclipped ? ink : boxes.boxes[layer.image.clip];
    if (layer.image.kind == PrLayoutImageKind_None || prRectIsEmpty(out->area))
        return;
    PrLayoutBackgroundLayer image = layer.image;
    image.attachment = PrLayoutBackgroundAttachment_Scroll;
    out->list = prDrawListCreate();
    layoutBackgroundPaintLayer(compounds, image, boxes, out->list);
}

// Combines the drawn layers, from the top layer down, and a border, into one mask (css-masking-1/#layering): each
// layer's values become alpha by its mode, the layers composite from the bottom up by their operators as an isolated
// group, and the border then masks the result. Writes the mask's list and area to `out`. Returns false if the mask
// masks everything away.
bool combine(Array<DrawnLayer>& layers, const Array<PrDrawComposite>& operators, DrawnLayer* border,
             DrawnLayer* out) {
    *out = DrawnLayer{.mode = PrDrawMaskMode_Alpha, .space = PrColorSpace_Srgb};
    if (border == nullptr && layers.count == 1) {
        *out = layers.data[0];
        layers.data[0].list = nullptr;
        return out->list != nullptr && !prRectIsEmpty(out->area);
    }
    PrRect area{};
    for (uint32_t i = 0; i < layers.count; ++i)
        area = layers.data[i].list != nullptr ? prRectUnion(area, layers.data[i].area) : area;
    bool onlyBorder = layers.count == 0;
    if (onlyBorder && border != nullptr)
        area = border->area;
    if (prRectIsEmpty(area))
        return false;
    PrDrawList* combined = prDrawListCreate();
    PrDrawPaint white{.color = PR_COLOR16(1, 1, 1, 1)};
    for (uint32_t k = layers.count; k-- > 0;) {
        const DrawnLayer& layer = layers.data[k];
        PrDrawComposite composite = k + 1 == layers.count ? PrDrawComposite_SourceOver : operators.data[k];
        if (layer.list == nullptr) {
            // A transparent layer leaves what lies below it, or clears it.
            if (composite == PrDrawComposite_SourceIn || composite == PrDrawComposite_SourceOut)
                prDrawListRect(combined, area, PrDrawPaint{.color = white.color, .composite = PrDrawComposite_Clear});
            continue;
        }
        PrDrawMask mask = prDrawMaskFromList(layer.list, layer.area, layer.mode, layer.space);
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = area;
        group.composite = composite;
        group.mask = &mask;
        prDrawListPushGroup(combined, &group);
        prDrawListRect(combined, area, white);
        prDrawListPopGroup(combined);
    }
    if (border != nullptr) {
        // The border masks after the layers, as alpha that the layers' result is multiplied by.
        PrDrawMask mask = prDrawMaskFromList(border->list, border->area, border->mode, border->space);
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = area;
        group.composite = onlyBorder ? PrDrawComposite_SourceOver : PrDrawComposite_DestinationIn;
        group.mask = &mask;
        prDrawListPushGroup(combined, &group);
        prDrawListRect(combined, area, white);
        prDrawListPopGroup(combined);
    }
    out->list = combined;
    out->area = area;
    return true;
}

// Resolves the mask at `index` into `out`. Returns false if the box draws nothing.
bool resolveMask(const LayoutTree* tree, PrLayoutMaskIndex index, const LayoutBackgroundBoxes& boxes, PrRect ink,
                 const float* widths, LayoutResolvedEffects* out) {
    const LayoutStyles* styles = tree->styles;
    const LayoutEffectEntry& entry = layoutEffectAt(&styles->effectLists, LayoutEffectKind_Mask, index);
    const PrLayoutMask& mask = entry.mask;
    PrRect border = boxes.boxes[PrLayoutBackgroundBox_BorderBox];
    Array<DrawnLayer> layers;
    Array<PrDrawComposite> operators;
    bool draws = true;
    for (uint32_t i = 0; i < mask.layerCount && draws; ++i) {
        const PrLayoutMaskLayer& layer = mask.layers[i];
        DrawnLayer drawn{.mode = PrDrawMaskMode_Alpha, .space = PrColorSpace_Srgb};
        if (layer.source.functions != nullptr) {
            // A source draws its mask in place, and decides its own mode unless the layer's mode overrides it.
            if (layer.source.functions->draw == nullptr)
                continue;
            PrDrawList* list = prDrawListCreate();
            PrLayoutSourceMask made{.mode = PrDrawMaskMode_Luminance, .space = PrColorSpace_Srgb};
            draws = layer.source.functions->draw(layer.source.context, border, list, &made);
            drawn = DrawnLayer{list, made.area, made.mode, made.space};
            if (layer.mode == PrLayoutMaskMode_Alpha)
                drawn.mode = PrDrawMaskMode_Alpha;
            else if (layer.mode == PrLayoutMaskMode_Luminance)
                drawn.mode = PrDrawMaskMode_Luminance;
            if (prRectIsEmpty(drawn.area)) {
                prDrawListRelease(list);
                drawn.list = nullptr;
            }
        } else {
            drawImageLayer(&styles->compounds, layer, boxes, ink, &drawn);
        }
        layers.push(drawn);
        operators.push(layerOperator(layer.composite));
    }
    DrawnLayer drawnBorder{};
    bool bordered = draws && mask.border.image.kind != PrLayoutImageKind_None
                    && drawBorder(&styles->compounds, mask.border, border, widths, &drawnBorder);
    DrawnLayer combined{};
    if (draws && (layers.count != 0 || bordered))
        draws = combine(layers, operators, bordered ? &drawnBorder : nullptr, &combined);
    for (uint32_t i = 0; i < layers.count; ++i)
        prDrawListRelease(layers.data[i].list);
    prDrawListRelease(drawnBorder.list);
    layers.reset();
    operators.reset();
    if (!draws) {
        prDrawListRelease(combined.list);
        return false;
    }
    out->mask = combined.list;
    out->maskArea = combined.area;
    out->maskMode = combined.mode;
    out->maskSpace = combined.space;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

// The boxes of a box with effects, in the space of its border box, which reference boxes name, and the radii of the
// border box's corners.
struct EffectBoxes {
    LayoutBackgroundBoxes boxes;
    PrRect margin;
    float widths[PrRectSide_Count];     // the border's widths
    float paddings[PrRectSide_Count];
    float margins[PrRectSide_Count];
};

// Returns the boxes of the box of `node`, whose border box is `border`, styled by `box`. An inline box's percentages of
// padding and margins are of zero.
EffectBoxes boxesOf(const LayoutTree* tree, const LayoutFragment* PR_NULLABLE fragment, const PrLayoutBoxStyle& box,
                    PrRect border) {
    const LayoutStyles* styles = tree->styles;
    EffectBoxes out{};
    PrLayoutUnit base = fragment != nullptr ? layoutUnitMax(fragment->box.containingInline, kLayoutUnitZero)
                                            : kLayoutUnitZero;
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        out.widths[side] = prLayoutUnitToPx(box.border[side]);
        out.paddings[side] = prLayoutUnitToPx(layoutResolve(styles, box.padding[side], base));
        out.margins[side] = layoutLengthIsAuto(box.margin[side])
                                ? 0
                                : prLayoutUnitToPx(layoutResolve(styles, box.margin[side], base));
    }
    auto inset = [](PrRect r, const float* by, float sign) {
        return PrRect{r.left + sign * by[PrRectSide_Left], r.top + sign * by[PrRectSide_Top],
                      r.right - sign * by[PrRectSide_Right], r.bottom - sign * by[PrRectSide_Bottom]};
    };
    out.boxes.boxes[PrLayoutBackgroundBox_BorderBox] = border;
    out.boxes.boxes[PrLayoutBackgroundBox_PaddingBox] = inset(border, out.widths, 1);
    out.boxes.boxes[PrLayoutBackgroundBox_ContentBox] = inset(out.boxes.boxes[PrLayoutBackgroundBox_PaddingBox],
                                                              out.paddings, 1);
    out.margin = inset(border, out.margins, -1);
    return out;
}

// Returns the shape of the reference box `which`, with its corners (css-shapes-1/#shapes-from-box-values): the border
// box's corners, reduced by the border's widths for the padding box and by the padding too for the content box, and
// grown by the margins for the margin box as a shadow's spread grows them.
PrCornerRect referenceShape(const LayoutStyles* styles, const PrLayoutDecorationStyle& decoration,
                            const EffectBoxes& boxes, PrLayoutGeometryBox which) {
    PrCornerRect border =
        layoutBorderShape(styles, decoration, boxes.boxes.boxes[PrLayoutBackgroundBox_BorderBox]);
    switch (which) {
        case PrLayoutGeometryBox_PaddingBox: return prCornerRectInset(border, boxes.widths);
        case PrLayoutGeometryBox_ContentBox: {
            float insets[PrRectSide_Count];
            for (uint32_t side = 0; side < PrRectSide_Count; ++side)
                insets[side] = boxes.widths[side] + boxes.paddings[side];
            return prCornerRectInset(border, insets);
        }
        case PrLayoutGeometryBox_MarginBox: {
            // A negative margin moves its side inward, and leaves the corners as they are.
            float outsets[PrRectSide_Count];
            for (uint32_t side = 0; side < PrRectSide_Count; ++side)
                outsets[side] = boxes.margins[side] > 0 ? boxes.margins[side] : 0;
            PrCornerRect margin = prCornerRectOutset(border, outsets);
            margin.rect = boxes.margin;
            return prCornerRectFit(margin);
        }
        default: return border;
    }
}

// Returns the reference box `which` of `boxes`.
PrRect referenceBox(const EffectBoxes& boxes, PrLayoutGeometryBox which) {
    return which == PrLayoutGeometryBox_MarginBox ? boxes.margin : boxes.boxes.boxes[which];
}

// The border box of the box of `node` in the space of the layer that paints it: its fragment's extent from the origin,
// or for an inline box, the bounds of its parts and rows in its block container's content, from the line `opening`.
PrRect borderOf(const LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment, LayoutPieceAt opening) {
    if (fragment->node == node)
        return prLayoutRectToPx(layoutRectAt(kLayoutVecZero, fragment->extent));
    PrLayoutRect bounds{};
    bool any = false;
    LayoutPieces pieces = layoutPiecesFrom(fragment, kLayoutVecZero, node, opening);
    LayoutPiece piece{};
    while (layoutPiecesNext(pieces, &piece)) {
        auto add = [&](PrLayoutRect rect) {
            bounds = any ? layoutRectUnion(bounds, rect) : rect;
            any = true;
        };
        if (piece.kind == LayoutPieceKind_Row) {
            add(layoutRectAt(piece.offset, piece.fragment->extent));
            continue;
        }
        for (uint32_t k = piece.first; k < piece.parts; ++k) {
            const LayoutChild& part = piece.fragment->children[k];
            add(layoutRectAt(piece.offset + part.offset, part.fragment->extent));
        }
    }
    (void)tree;
    return prLayoutRectToPx(bounds);
}

// Returns the rectangle of the clip property, in `border`'s space (css-masking-1/#clip-property).
PrRect clipRectOf(const LayoutCompounds* compounds, const PrLayoutCompositingStyle& c, PrRect border) {
    float width = prRectWidth(border);
    float height = prRectHeight(border);
    auto edge = [&](uint32_t side, float fallback) {
        return layoutLengthIsAuto(c.clip[side]) ? fallback
                                                : resolvePx(compounds, c.clip[side], side % 2 == 0 ? height : width);
    };
    return PrRect{border.left + edge(PrRectSide_Left, 0), border.top + edge(PrRectSide_Top, 0),
                  border.left + edge(PrRectSide_Right, width), border.top + edge(PrRectSide_Bottom, height)};
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Interning

void layoutEffectListsInit(LayoutEffectLists* lists) {
    for (LayoutEffectTable& table : lists->tables)
        table.entries.push(LayoutEffectEntry{});
}

PrLayoutShapeIndex layoutEffectListsInternShape(LayoutEffectLists* lists, LayoutCompounds* compounds,
                                                const PrLayoutShape& shape) {
    PrLayoutShape own = shape;
    own.pointCount = shape.pointCount < kPrLayoutShapeMaxPoints ? shape.pointCount : uint32_t(kPrLayoutShapeMaxPoints);
    Array<uint8_t> key;
    shapeKey(key, own);
    return intern(lists, compounds, LayoutEffectKind_Shape, key, &own, nullptr, nullptr, 0);
}

PrLayoutMaskIndex layoutEffectListsInternMask(LayoutEffectLists* lists, LayoutCompounds* compounds,
                                              const PrLayoutMask& mask) {
    if (mask.layerCount == 0 && mask.border.image.kind == PrLayoutImageKind_None)
        return kPrLayoutMaskNone;
    Array<uint8_t> key;
    maskKey(key, mask);
    return intern(lists, compounds, LayoutEffectKind_Mask, key, nullptr, &mask, nullptr, 0);
}

PrLayoutFilterIndex layoutEffectListsInternFilter(LayoutEffectLists* lists, LayoutCompounds* compounds,
                                                  const PrLayoutFilter* filters, uint32_t count) {
    if (count == 0)
        return kPrLayoutFilterNone;
    Array<uint8_t> key;
    layoutFilterKey(key, filters, count);
    return intern(lists, compounds, LayoutEffectKind_Filter, key, nullptr, nullptr, filters, count);
}

void layoutEffectListsCount(LayoutEffectLists* lists, LayoutCompounds* compounds,
                            const PrLayoutCompositingStyle& compositing, uint32_t delta) {
    countAt(lists->tables[LayoutEffectKind_Shape], compositing.clipShape, delta);
    countAt(lists->tables[LayoutEffectKind_Mask], compositing.mask, delta);
    countAt(lists->tables[LayoutEffectKind_Filter], compositing.filter, delta);
    countAt(lists->tables[LayoutEffectKind_Filter], compositing.backdrop, delta);
    for (PrLayoutLength length : compositing.clip) {
        PR_ASSERT(layoutLengthIsAuto(length) || prLayoutWordKind(length.bits) < PrLayoutWordKind_Number
                  || prLayoutWordKind(length.bits) == PrLayoutWordKind_Compound);
        layoutCompoundsCount(compounds, length.bits, delta);
    }
    if (compositing.clipKind == PrLayoutClipKind_Source)
        layoutSourceCount(compositing.clipSource, delta);
}

void layoutEffectListsCollect(LayoutEffectLists* lists, LayoutCompounds* compounds) {
    for (uint32_t kind = 0; kind < 3; ++kind) {
        LayoutEffectTable& table = lists->tables[kind];
        for (uint32_t k = 0; k < table.candidates.count; ++k) {
            uint32_t i = table.candidates.data[k];
            LayoutEffectEntry& e = table.entries.data[i];
            if (!e.live || e.references != 0)
                continue;
            table.index.erase(e.hash, i);
            countEntry(compounds, LayoutEffectKind(kind), e, 0xffffffffu);
            freeEntry(e);
            table.free.push(i);
        }
        table.candidates.clear();
    }
}

bool layoutEffectListsVerify(const LayoutEffectLists* lists) {
    for (const LayoutEffectTable& table : lists->tables) {
        bool covered = layoutCandidatesCover(table.candidates, table.entries.count, [&](uint32_t i) {
            const LayoutEffectEntry& e = table.entries.data[i];
            return e.live && e.references == 0;
        });
        if (!covered)
            return false;
    }
    return true;
}

void layoutEffectListsReset(LayoutEffectLists* lists) {
    for (uint32_t kind = 0; kind < 3; ++kind) {
        LayoutEffectTable& table = lists->tables[kind];
        for (uint32_t i = 0; i < table.entries.count; ++i) {
            LayoutEffectEntry& e = table.entries.data[i];
            if (!e.live)
                continue;
            // Compounds are freed with the styles, so only what the entry holds outside them is released.
            if (kind == LayoutEffectKind_Mask) {
                for (uint32_t l = 0; l < e.mask.layerCount; ++l) {
                    releaseImage(e.mask.layers[l].image);
                    layoutSourceCount(e.mask.layers[l].source, 0xffffffffu);
                }
                releaseImage(e.mask.border.image);
            } else if (kind == LayoutEffectKind_Filter) {
                for (uint32_t f = 0; f < e.filterCount; ++f)
                    layoutSourceCount(e.filters[f].source, 0xffffffffu);
            }
            freeEntry(e);
        }
        table.entries.reset();
        table.free.reset();
        table.candidates.reset();
        table.index.reset();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Questions

bool layoutCompositingGroups(const PrLayoutCompositingStyle& compositing) {
    const PrLayoutCompositingStyle& c = compositing;
    return c.clipKind != PrLayoutClipKind_None || c.mask != kPrLayoutMaskNone || c.filter != kPrLayoutFilterNone
           || c.backdrop != kPrLayoutFilterNone || c.blendMode != PrDrawBlendMode_Normal
           || c.composite != PrDrawComposite_SourceOver || c.isolated || c.backdropRoot;
}

bool layoutStyleBlends(const LayoutStyles* styles, PrLayoutStyleIndex style) {
    if (layoutStyle(styles, style).groups[PrLayoutGroup_Compositing] == 0)
        return false;
    const PrLayoutCompositingStyle& c = layoutCompositingStyle(styles, style);
    return c.blendMode != PrDrawBlendMode_Normal || c.composite != PrDrawComposite_SourceOver;
}

bool layoutIsolatesBlending(const LayoutTree* tree, LayoutNodeIndex node) {
    if (tree->styles->blendingGroups == 0 || !layoutNodeLeadsWhole(tree, node) || !layoutIsStackingContext(tree, node))
        return false;
    LayoutStackedBoxes list = layoutStackingList(tree, node);
    for (uint32_t i = 0; i < list.count; ++i) {
        if (layoutStyleBlends(tree->styles, layoutTreeNode(tree, list.boxes[i].node).style))
            return true;
    }
    return false;
}

bool layoutHasCorners(const PrLayoutDecorationStyle& decoration) {
    for (PrLayoutLength length : decoration.radii) {
        if (length.bits != prLayoutLengthPx(0).bits)
            return true;
    }
    return false;
}

PrCornerRect layoutBorderShape(const LayoutStyles* styles, const PrLayoutDecorationStyle& decoration, PrRect border,
                               uint8_t broken) {
    // The sides that meet at each corner.
    static const uint8_t kSides[PrRectCorner_Count] = {1u << PrRectSide_Top | 1u << PrRectSide_Left,
                                                        1u << PrRectSide_Top | 1u << PrRectSide_Right,
                                                        1u << PrRectSide_Bottom | 1u << PrRectSide_Right,
                                                        1u << PrRectSide_Bottom | 1u << PrRectSide_Left};
    PrCornerRect shape{.rect = border};
    cornersOf(&styles->compounds, decoration.radii, PrVec2{prRectWidth(border), prRectHeight(border)}, shape.corners);
    for (uint32_t c = 0; c < PrRectCorner_Count; ++c) {
        shape.corners[c].shape = decoration.cornerShapes[c];
        if ((broken & kSides[c]) != 0)
            shape.corners[c].radii = PrVec2{0, 0};
    }
    return prCornerRectFit(shape);
}

bool layoutClipCorners(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutRect clip,
                       PrCorner out[PrRectCorner_Count]) {
    const LayoutStyles* styles = tree->styles;
    PrLayoutStyleIndex style = layoutTreeNode(tree, fragment->node).style;
    const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(styles, style);
    if (!layoutHasCorners(decoration))
        return false;
    const PrLayoutBoxStyle& box = layoutBoxStyle(styles, style);
    PrLayoutRect border{clip.left - box.border[PrRectSide_Left], clip.top - box.border[PrRectSide_Top],
                        clip.right + box.border[PrRectSide_Right], clip.bottom + box.border[PrRectSide_Bottom]};
    if (layoutRectWidth(border) != fragment->extent.x || layoutRectHeight(border) != fragment->extent.y)
        return false;
    float widths[PrRectSide_Count];
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        widths[side] = prLayoutUnitToPx(box.border[side]);
    PrCornerRect padding = prCornerRectInset(layoutBorderShape(styles, decoration, prLayoutRectToPx(border)), widths);
    bool curved = false;
    for (uint32_t c = 0; c < PrRectCorner_Count; ++c) {
        out[c] = padding.corners[c];
        curved = curved || (out[c].radii.x > 0 && out[c].radii.y > 0);
    }
    return curved;
}

bool layoutBorderContains(const LayoutStyles* styles, PrLayoutStyleIndex style, PrLayoutRect border, PrLayoutVec at) {
    if (!layoutRectContains(border, at))
        return false;
    const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(styles, style);
    if (!layoutHasCorners(decoration))
        return true;
    PrCornerRect shape = layoutBorderShape(styles, decoration, prLayoutRectToPx(border));
    return prCornerRectContains(shape, PrVec2{prLayoutUnitToPx(at.x), prLayoutUnitToPx(at.y)});
}

PrLayoutRect layoutClipRect(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutRect border) {
    const LayoutStyles* styles = tree->styles;
    const PrLayoutCompositingStyle& c = layoutCompositingStyle(styles, layoutTreeNode(tree, node).style);
    return prLayoutRectFromPx(clipRectOf(&styles->compounds, c, prLayoutRectToPx(border)));
}

bool layoutCompositingIsBackdropRoot(const PrLayoutCompositingStyle& compositing) {
    const PrLayoutCompositingStyle& c = compositing;
    return c.clipKind != PrLayoutClipKind_None || c.mask != kPrLayoutMaskNone || c.filter != kPrLayoutFilterNone
           || c.backdrop != kPrLayoutFilterNone || c.blendMode != PrDrawBlendMode_Normal
           || c.composite != PrDrawComposite_SourceOver || c.backdropRoot;
}

PrDrawFilterInput layoutFilterAppend(const PrLayoutFilter& filter, PrDrawFilterInput input, PrImageEdgeMode edge,
                                     PrDrawFilterBuilder* builder) {
    PR_ASSERT(filter.kind != PrLayoutFilterKind_Source);
    return appendFunction(filter, input, edge, builder);
}

void layoutShapePath(const LayoutCompounds* PR_NULLABLE compounds, const PrLayoutShape& shape, PrRect box,
                     PrPath* out) {
    switch (shape.kind) {
        case PrLayoutShapeKind_Inset: insetPath(compounds, shape, box, out); break;
        case PrLayoutShapeKind_Circle:
        case PrLayoutShapeKind_Ellipse: ellipsePath(compounds, shape, box, out); break;
        case PrLayoutShapeKind_Polygon: polygonPath(compounds, shape, box, out); break;
        case PrLayoutShapeKind_Path:
            if (shape.path != nullptr)
                prPathAddPath(out, shape.path, prAffine2Translate(PrVec2{box.left, box.top}));
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Resolving a box's effects

bool layoutEffectsResolve(const LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment,
                          LayoutPieceAt opening, PrRect ink, LayoutResolvedEffects* out) {
    *out = LayoutResolvedEffects{};
    const LayoutStyles* styles = tree->styles;
    PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
    if (layoutStyle(styles, style).groups[PrLayoutGroup_Compositing] == 0)
        return false;
    const PrLayoutCompositingStyle& c = layoutCompositingStyle(styles, style);
    const PrLayoutBoxStyle& box = layoutBoxStyle(styles, style);
    const LayoutCompounds* compounds = &styles->compounds;
    bool inlineBox = fragment->node != node;
    PrRect border = borderOf(tree, node, fragment, opening);
    EffectBoxes boxes = boxesOf(tree, inlineBox ? nullptr : fragment, box, border);
    out->blendMode = c.blendMode;
    out->composite = c.composite;
    out->isolated = c.isolated;
    out->backdropRoot = layoutCompositingIsBackdropRoot(c);
    if (box.clipped) {
        out->clipsRect = true;
        out->clipRect = clipRectOf(compounds, c, border);
    }
    switch (c.clipKind) {
        case PrLayoutClipKind_Shape: {
            PrPath* path = prPathCreate();
            const LayoutEffectEntry& shape = layoutEffectAt(&styles->effectLists, LayoutEffectKind_Shape, c.clipShape);
            layoutShapePath(compounds, shape.shape, referenceBox(boxes, c.clipBox), path);
            out->clipRule = shape.shape.rule;
            if (prPathVerbCount(path) == 0) {
                prPathDestroy(path);
                out->hidden = true;
                return true;
            }
            out->clipPath = path;
            break;
        }
        case PrLayoutClipKind_Box: {
            PrCornerRect shape = referenceShape(styles, layoutDecorationStyle(styles, style), boxes, c.clipBox);
            if (prRectIsEmpty(shape.rect)) {
                out->hidden = true;
                return true;
            }
            PrPath* path = prPathCreate();
            prPathAddCornerRect(path, shape);
            out->clipPath = path;
            out->clipRule = PrPathFillRule_NonZero;
            break;
        }
        case PrLayoutClipKind_Source: {
            const PrLayoutSourceFunctions* functions = c.clipSource.functions;
            if (functions == nullptr || functions->draw == nullptr)
                break;
            PrDrawList* list = prDrawListCreate();
            PrLayoutSourceMask made{.mode = PrDrawMaskMode_Alpha, .space = PrColorSpace_Srgb};
            if (!functions->draw(c.clipSource.context, border, list, &made) || prRectIsEmpty(made.area)) {
                prDrawListRelease(list);
                out->hidden = true;
                return true;
            }
            prDrawListFreeze(list);
            out->coverage = list;
            out->coverageArea = made.area;
            break;
        }
        default: break;
    }
    if (c.mask != kPrLayoutMaskNone && !resolveMask(tree, c.mask, boxes.boxes, ink, boxes.widths, out)) {
        layoutEffectsReset(out);
        out->hidden = true;
        return true;
    }
    bool hidden = false;
    out->filter = filterOf(&styles->effectLists, c.filter, border, prRectUnion(border, ink), false, PrRect{},
                           &out->filterRegion, &hidden);
    // The root is the outermost backdrop root, so nothing lies beneath it within one, and its backdrop filter reads
    // nothing (filter-effects-2/#BackdropRoot).
    bool root = layoutTreeNode(tree, node).parent == kPrLayoutNodeNone;
    if (!hidden && !root && c.backdrop != kPrLayoutFilterNone) {
        PrRect unused{};
        out->backdropShape = layoutBorderShape(styles, layoutDecorationStyle(styles, style), border);
        out->backdrop = filterOf(&styles->effectLists, c.backdrop, border, border, true, border, &unused, &hidden);
    }
    if (hidden) {
        layoutEffectsReset(out);
        out->hidden = true;
    }
    return true;
}

void layoutEffectsReset(LayoutResolvedEffects* effects) {
    prPathDestroy(effects->clipPath);
    prDrawListRelease(effects->coverage);
    prDrawListRelease(effects->mask);
    prDrawFilterRelease(effects->filter);
    prDrawFilterRelease(effects->backdrop);
    *effects = LayoutResolvedEffects{};
}

LayoutEffectsPushed layoutEffectsPush(const LayoutResolvedEffects& effects, float opacity, bool isolate,
                                      PrDrawList* list) {
    const LayoutResolvedEffects& e = effects;
    PR_ASSERT(!e.hidden);
    LayoutEffectsPushed pushed{};
    if (e.clipsRect) {
        prDrawListPushClipRect(list, e.clipRect, PrDrawEdges_Snapped);
        ++pushed.clips;
    }
    if (e.clipPath != nullptr) {
        prDrawListPushClipPath(list, e.clipPath, e.clipRule, PrDrawEdges_Antialiased);
        ++pushed.clips;
    }
    // A clip path that a source drew is its coverage, which masks a group around the rest.
    if (e.coverage != nullptr) {
        PrDrawMask mask = prDrawMaskFromList(e.coverage, e.coverageArea, PrDrawMaskMode_Alpha, PrColorSpace_Srgb);
        PrDrawGroup group = prDrawGroupDefault();
        group.mask = &mask;
        prDrawListPushGroup(list, &group);
        ++pushed.groups;
    }
    bool grouped = opacity < 1 || e.filter != nullptr || e.backdrop != nullptr || e.mask != nullptr
                   || e.blendMode != PrDrawBlendMode_Normal || e.composite != PrDrawComposite_SourceOver
                   || e.isolated || e.backdropRoot || isolate;
    if (!grouped)
        return pushed;
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = opacity;
    group.blendMode = e.blendMode;
    group.composite = e.composite;
    group.isolated = true;
    group.filter = e.filter;
    group.bounds = e.filterRegion;
    group.backdrop = e.backdrop;
    PrDrawMask mask{};
    if (e.mask != nullptr) {
        mask = prDrawMaskFromList(e.mask, e.maskArea, e.maskMode, e.maskSpace);
        group.mask = &mask;
    }
    prDrawListPushGroup(list, &group);
    ++pushed.groups;
    if (e.backdrop != nullptr) {
        // The backdrop shows within the border box and its corners (filter-effects-2/#backdrop-filter-operation), so
        // the group's content starts with the backdrop erased outside them.
        PrPath* outside = prPathCreate();
        prPathAddRect(outside, PrRect{-kEverywhere, -kEverywhere, kEverywhere, kEverywhere});
        prPathAddCornerRect(outside, e.backdropShape);
        prDrawListPath(list, outside, PrPathFillRule_EvenOdd,
                       PrDrawPaint{.color = PR_COLOR16(1, 1, 1, 1), .composite = PrDrawComposite_DestinationOut});
        prPathDestroy(outside);
    }
    return pushed;
}

void layoutEffectsPop(LayoutEffectsPushed pushed, PrDrawList* list) {
    for (uint32_t i = 0; i < pushed.groups; ++i)
        prDrawListPopGroup(list);
    for (uint32_t i = 0; i < pushed.clips; ++i)
        prDrawListPopClip(list);
}

bool layoutEffectsContain(const LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment, PrVec2 at) {
    const LayoutStyles* styles = tree->styles;
    PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
    if (layoutStyle(styles, style).groups[PrLayoutGroup_Compositing] == 0)
        return true;
    const PrLayoutCompositingStyle& c = layoutCompositingStyle(styles, style);
    const PrLayoutBoxStyle& box = layoutBoxStyle(styles, style);
    PrRect border = prLayoutRectToPx(layoutRectAt(kLayoutVecZero, fragment->extent));
    switch (c.clipKind) {
        case PrLayoutClipKind_Shape:
        case PrLayoutClipKind_Box: {
            EffectBoxes boxes = boxesOf(tree, fragment, box, border);
            PrPath* path = prPathCreate();
            PrPathFillRule rule = PrPathFillRule_NonZero;
            if (c.clipKind == PrLayoutClipKind_Shape) {
                const LayoutEffectEntry& shape =
                    layoutEffectAt(&styles->effectLists, LayoutEffectKind_Shape, c.clipShape);
                layoutShapePath(&styles->compounds, shape.shape, referenceBox(boxes, c.clipBox), path);
                rule = shape.shape.rule;
            } else {
                const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(styles, style);
                prPathAddCornerRect(path, referenceShape(styles, decoration, boxes, c.clipBox));
            }
            bool inside = prPathContains(path, at, rule);
            prPathDestroy(path);
            return inside;
        }
        case PrLayoutClipKind_Source: {
            const PrLayoutSourceFunctions* functions = c.clipSource.functions;
            return functions == nullptr || functions->contains == nullptr
                   || functions->contains(c.clipSource.context, border, at);
        }
        default: return true;
    }
}

} // namespace pr_impl
