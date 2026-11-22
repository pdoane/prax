// Drawing: the vocabulary's values and predicates.

#include "pr/draw.h"

#include "pr/float.h"
#include "pr/vec.h"

#include "draw/src/draw_list.h"

namespace pr_impl {

namespace {

PrDrawBrush gradient(PrDrawBrushKind kind, const PrDrawColorStop* stops, uint32_t stopCount) {
    PrDrawBrush brush = prDrawBrushDefault(kind);
    brush.stops = stops;
    brush.stopCount = stopCount;
    return brush;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prDrawCompositeIsUnbounded(PrDrawComposite composite) {
    switch (composite) {
        case PrDrawComposite_Clear:
        case PrDrawComposite_Copy:
        case PrDrawComposite_SourceIn:
        case PrDrawComposite_SourceOut:
        case PrDrawComposite_DestinationIn:
        case PrDrawComposite_DestinationAtop:
            return true;
        default:
            return false;
    }
}

PrDrawSource prDrawSourceList(PrDrawList* list, PrRect rect) {
    PrDrawSource source{};
    drawListFreeze(list);
    source.kind = PrDrawSourceKind_List;
    source.rect = rect;
    source.list = list;
    return source;
}

PrDrawSource prDrawSourceTexture(PrTexture texture, PrUVec2 extent, PrRect rect) {
    PrDrawSource source{};
    source.kind = PrDrawSourceKind_Texture;
    source.alpha = texture.alpha;
    source.rect = rect;
    source.extent = extent;
    source.texture = texture.texture;
    return source;
}

PrDrawSource prDrawSourceRegion(const PrTextureRegion* region, PrRect rect, PrTextureOrientationFlags orientation) {
    // A source names one layer of its texture, the first.
    PR_ASSERT(region->layer == 0);
    return prDrawSourceTexture(region->texture, region->extent, prTextureRegionRect(region, rect, orientation));
}

bool prDrawSourceIsValid(PrDrawSource source) {
    if (prRectIsEmpty(source.rect))
        return false;
    switch (source.kind) {
        case PrDrawSourceKind_List: return source.list != nullptr;
        case PrDrawSourceKind_Texture: return source.texture.id != 0;
    }
    return false;
}

PrUVec2 prDrawSourceTexels(PrDrawSource source) {
    float width = prF32Round(source.rect.right - source.rect.left);
    float height = prF32Round(source.rect.bottom - source.rect.top);
    return PrUVec2{width > 0 ? uint32_t(width) : 0, height > 0 ? uint32_t(height) : 0};
}

PrDrawBrush prDrawBrushDefault(PrDrawBrushKind kind) {
    PrDrawBrush brush{};
    brush.kind = kind;
    brush.space = PrDrawBrushSpace_User;
    brush.extend = PrDrawExtend_Pad;
    brush.interpolationSpace = PrColorSpace_Srgb;
    brush.hueMethod = PrColorHueMethod_Shorter;
    brush.transform = prAffine2Identity();
    brush.opacity = 1;
    if (kind == PrDrawBrushKind_Image) {
        brush.image.tiles.x = PrDrawExtend_Pad;
        brush.image.tiles.y = PrDrawExtend_Pad;
        brush.image.sampling = PrDrawSampling_Linear;
    }
    return brush;
}

PrDrawBrush prDrawBrushImage(PrDrawSource source, PrRect destination, PrDrawSampling sampling) {
    PrDrawBrush brush = prDrawBrushDefault(PrDrawBrushKind_Image);
    brush.image.source = source;
    brush.image.tiles.tile = destination;
    brush.image.sampling = sampling;
    return brush;
}

void prDrawBrushImageUpright(PrDrawBrush* brush, PrTextureOrientationFlags orientation, PrRect region) {
    if (brush->kind != PrDrawBrushKind_Image || orientation == 0)
        return;
    bool transposed = (orientation & PrTextureOrientationFlag_Transpose) != 0;
    bool flipX = (orientation & PrTextureOrientationFlag_FlipX) != 0;
    bool flipY = (orientation & PrTextureOrientationFlag_FlipY) != 0;
    float regionWidth = region.right - region.left;
    float regionHeight = region.bottom - region.top;
    PrVec2 displayed = transposed ? PrVec2{regionHeight, regionWidth} : PrVec2{regionWidth, regionHeight};
    // The source's rect is in the displayed orientation. Taken relative to the region, then unmirrored, then
    // transposed, it is the rect of the stored texels.
    PrRect rect = brush->image.source.rect;
    rect = PrRect{rect.left - region.left, rect.top - region.top, rect.right - region.left, rect.bottom - region.top};
    if (flipX)
        rect = PrRect{displayed.x - rect.right, rect.top, displayed.x - rect.left, rect.bottom};
    if (flipY)
        rect = PrRect{rect.left, displayed.y - rect.bottom, rect.right, displayed.y - rect.top};
    if (transposed)
        rect = PrRect{rect.top, rect.left, rect.bottom, rect.right};
    brush->image.source.rect =
        PrRect{rect.left + region.left, rect.top + region.top, rect.right + region.left, rect.bottom + region.top};
    // The stored texels span a tile at the same origin as the displayed tile. It has the displayed tile's size, with
    // the width and height exchanged if the orientation transposes. The transform maps a point of the stored tile onto
    // the displayed tile as the orientation moves it: transposed, then mirrored within the tile.
    PrDrawTiles& tiles = brush->image.tiles;
    PrRect tile = tiles.tile;
    float width = tile.right - tile.left;
    float height = tile.bottom - tile.top;
    PrRect stored = transposed ? PrRect{tile.left, tile.top, tile.left + height, tile.top + width} : tile;
    PrAffine2 local{1, 0, 0, 1, -tile.left, -tile.top};
    PrAffine2 swap = transposed ? PrAffine2{0, 1, 1, 0, 0, 0} : PrAffine2{1, 0, 0, 1, 0, 0};
    PrAffine2 mirror{flipX ? -1.0f : 1.0f, 0, 0, flipY ? -1.0f : 1.0f, flipX ? width : 0, flipY ? height : 0};
    PrAffine2 place{1, 0, 0, 1, tile.left, tile.top};
    PrAffine2 toTile = prAffine2Concat(prAffine2Concat(prAffine2Concat(local, swap), mirror), place);
    brush->transform = prAffine2Concat(toTile, brush->transform);
    tiles.tile = stored;
    if (transposed) {
        PrDrawExtend x = tiles.x;
        tiles.x = tiles.y;
        tiles.y = x;
        tiles.spacing = PrVec2{tiles.spacing.y, tiles.spacing.x};
    }
}

PrDrawBrush prDrawBrushLinear(PrVec2 start, PrVec2 end, const PrDrawColorStop* stops, uint32_t stopCount) {
    PrDrawBrush brush = gradient(PrDrawBrushKind_Linear, stops, stopCount);
    brush.linear.start = start;
    brush.linear.end = end;
    return brush;
}

PrDrawBrush prDrawBrushRadial(PrVec2 start, float startRadius, PrVec2 end, float endRadius,
        const PrDrawColorStop* stops, uint32_t stopCount) {
    PrDrawBrush brush = gradient(PrDrawBrushKind_Radial, stops, stopCount);
    brush.radial.start = start;
    brush.radial.startRadius = startRadius;
    brush.radial.end = end;
    brush.radial.endRadius = endRadius;
    return brush;
}

PrDrawBrush prDrawBrushConic(PrVec2 center, float angle, const PrDrawColorStop* stops, uint32_t stopCount) {
    PrDrawBrush brush = gradient(PrDrawBrushKind_Conic, stops, stopCount);
    brush.conic.center = center;
    brush.conic.angle = angle;
    return brush;
}

PrDrawBrush prDrawBrushDiamond(PrVec2 center, PrVec2 radii, const PrDrawColorStop* stops, uint32_t stopCount) {
    PrDrawBrush brush = gradient(PrDrawBrushKind_Diamond, stops, stopCount);
    brush.diamond.center = center;
    brush.diamond.radii = radii;
    return brush;
}

PrDrawStroke prDrawStrokeDefault(void) {
    PrDrawStroke stroke{};
    stroke.shape = prPathStrokeDefault();
    return stroke;
}

PrDrawMask prDrawMaskDefault(const PrDrawBrush* brush) {
    PrDrawMask mask{};
    mask.brush = *brush;
    return mask;
}

PrDrawMask prDrawMaskFromList(PrDrawList* list, PrRect area, PrDrawMaskMode mode, PrColorSpace space) {
    PrDrawBrush brush = prDrawBrushImage(prDrawSourceList(list, area), area, PrDrawSampling_Linear);
    brush.image.tiles.x = PrDrawExtend_None;
    brush.image.tiles.y = PrDrawExtend_None;
    PrDrawMask mask = prDrawMaskDefault(&brush);
    mask.mode = mode;
    mask.space = space;
    return mask;
}

PrDrawGroup prDrawGroupDefault(void) {
    PrDrawGroup group{};
    group.opacity = 1;
    return group;
}

bool prDrawGroupHasEffect(const PrDrawGroup* group) {
    return !(group->opacity >= 1) || group->composite != PrDrawComposite_SourceOver ||
        group->blendMode != PrDrawBlendMode_Normal || group->filter != nullptr ||
        group->backdrop != nullptr || group->mask != nullptr || group->isolated ||
        group->knockout || group->projection != nullptr || group->space != PrColorSpace_Srgb || group->layer != nullptr;
}

bool prDrawGroupIsBackdropRoot(const PrDrawGroup* group) {
    PrDrawGroup flat = *group;
    flat.projection = nullptr;
    return prDrawGroupHasEffect(&flat);
}
