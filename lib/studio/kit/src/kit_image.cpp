// Studio Kit's images: pictures fitted into the boxes that they are placed in.

#include "pr/kit_image.h"

#include "pr/assert.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// Returns where an image sits along an axis of the room that its fit leaves, as a percentage of that room.
float alignPercent(PrKitAlign align) {
    return align == PrKitAlign_Start ? 0 : align == PrKitAlign_End ? 100 : 50;
}

// An image's decoration: how it fits its box, and where it sits in the room its fit leaves.
PR_NO_PADDING_BEGIN
struct ImageFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    PrKitImageFit fit;
    PrKitAlign alignX;
    PrKitAlign alignY;
    uint8_t _pad;

    void make(const Kit&, PrLayoutDecorationStyle& decoration) const {
        static const PrLayoutObjectFit kFits[] = {PrLayoutObjectFit_Fill, PrLayoutObjectFit_Contain,
                                                  PrLayoutObjectFit_Cover, PrLayoutObjectFit_None,
                                                  PrLayoutObjectFit_ScaleDown};
        decoration.objectFit = kFits[fit];
        decoration.objectPositionX = prLayoutLengthPercent(alignPercent(alignX));
        decoration.objectPositionY = prLayoutLengthPercent(alignPercent(alignY));
    }
};
PR_NO_PADDING_END

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitImage(PrKit* kit, PrLayoutNode parent, PrKitImage image, PrKitImageStyle style) {
    PR_ASSERT(style.fit <= PrKitImageFit_ScaleDown);
    PrLayoutTree* tree = kit->tree;
    ImageFill fill{.fit = style.fit, .alignX = style.alignX, .alignY = style.alignY};
    PrLayoutNode node =
        prLayoutTreeFrameReplaced(tree, parent, image.key, kitStyle(*kit, KitBlock{}, KitFixed{}, fill));
    PrUVec2 texels = prDrawImageExtent(image.image);
    PrLayoutNaturalSize natural{.width = prLayoutUnitFromPx(float(texels.x)),
                                .height = prLayoutUnitFromPx(float(texels.y)),
                                .ratio = texels.y != 0 ? float(texels.x) / float(texels.y) : 0};
    prLayoutTreeFrameSetNaturalSize(tree, node, natural);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Image;
    content.image = image.image;
    prLayoutTreeFrameSetContent(tree, node, &content);
    prKitPlace(kit, node, &style.place);
    return node;
}
