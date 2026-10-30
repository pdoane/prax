// Drawing: the vocabulary's values and predicates.

#include "pr/draw.h"

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

PrDrawSource prDrawSourceImage(const PrDrawImage* image, PrRect rect) {
    PrDrawSource source{};
    source.kind = PrDrawSourceKind_Image;
    source.rect = rect;
    source.image = image;
    return source;
}

PrDrawSource prDrawSourceList(PrDrawList* list, PrRect rect) {
    PrDrawSource source{};
    drawListFreeze(list);
    source.kind = PrDrawSourceKind_List;
    source.rect = rect;
    source.list = list;
    return source;
}

PrDrawSource prDrawSourceTexture(PrDrawTextureId texture, PrUVec2 extent, PrRect rect, PrDrawAlpha alpha) {
    PrDrawSource source{};
    source.kind = PrDrawSourceKind_Texture;
    source.alpha = alpha;
    source.rect = rect;
    source.extent = extent;
    source.texture = texture;
    return source;
}

bool prDrawSourceIsValid(PrDrawSource source) {
    if (prRectIsEmpty(source.rect))
        return false;
    switch (source.kind) {
        case PrDrawSourceKind_Image: return source.image != nullptr;
        case PrDrawSourceKind_List: return source.list != nullptr;
        case PrDrawSourceKind_Texture: return source.texture != 0;
    }
    return false;
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
