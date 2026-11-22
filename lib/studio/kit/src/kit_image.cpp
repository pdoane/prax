// Studio Kit's images: pictures fitted into the boxes that they are placed in.

#include "pr/kit_image.h"

#include "pr/assert.h"
#include "pr/draw_list.h"
#include "pr/hash.h"

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

    void make(const PrKit&, PrLayoutDecorationStyle& decoration) const {
        static const PrLayoutObjectFit kFits[] = {
            PrLayoutObjectFit_Fill, PrLayoutObjectFit_Contain,
            PrLayoutObjectFit_Cover, PrLayoutObjectFit_None,
            PrLayoutObjectFit_ScaleDown,
        };
        decoration.objectFit = kFits[fit];
        decoration.objectPositionX = prLayoutLengthPercent(alignPercent(alignX));
        decoration.objectPositionY = prLayoutLengthPercent(alignPercent(alignY));
    }
};
PR_NO_PADDING_END

// Creates the replaced element of an image of `texels` under `parent`, fitted as `style` says.
PrLayoutNode imageNode(PrKit* kit, PrLayoutNode parent, PrUiKey key, PrUVec2 texels, const PrKitImageStyle& style) {
    PR_ASSERT(style.fit <= PrKitImageFit_ScaleDown);
    PrLayoutTree* tree = kit->tree;
    ImageFill fill{.fit = style.fit, .alignX = style.alignX, .alignY = style.alignY};
    PrLayoutNode node =
        prLayoutTreeFrameReplaced(tree, parent, kitKey(*kit, parent, key), kitStyle(*kit, KitBlock{}, KitFixed{}, fill));
    PrLayoutNaturalSize natural{
        .width = prLayoutUnitFromPx(float(texels.x)),
        .height = prLayoutUnitFromPx(float(texels.y)),
        .ratio = texels.y != 0 ? float(texels.x) / float(texels.y) : 0,
    };
    prLayoutTreeFrameSetNaturalSize(tree, node, natural);
    return node;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitImage(PrKit* kit, PrLayoutNode parent, PrKitImage image, PrKitImageStyle style) {
    PrLayoutTree* tree = kit->tree;
    PrLayoutNode node =
        imageNode(kit, parent, image.key, prTextureDescDisplayedExtent(prTextureSourceDesc(image.image)), style);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Image;
    content.image = image.image;
    content.upright = true;
    prLayoutTreeFrameSetContent(tree, node, &content);
    prKitPlace(kit, node, style.place);
    return node;
}

PrLayoutNode prKitTexture(PrKit* kit, PrLayoutNode parent, PrKitTexture texture, PrKitImageStyle style) {
    PrLayoutTree* tree = kit->tree;
    PrLayoutNode node = imageNode(kit, parent, texture.key, texture.extent, style);
    // The list draws the texture over the content's natural space, and painting maps that space onto the box. A node
    // that persists from the previous frame may have shown another texture. The content's revision therefore combines
    // the texture, the extent and the application's revision, and the list is rebuilt when any of them changes.
    uint64_t revision = prHashMixKeys(prHashMixKeys(texture.texture.texture.id, texture.revision),
        (uint64_t(texture.extent.x) << 32) | texture.extent.y);
    PrLayoutContent current = prLayoutNodeContent(tree, node);
    if (current.kind != PrLayoutContentKind_DrawList || current.revision != revision) {
        PrDrawList* list = prDrawListCreate();
        PrRect texels{0, 0, float(texture.extent.x), float(texture.extent.y)};
        PrDrawSource source = prDrawSourceTexture(texture.texture, texture.extent, texels);
        PrDrawBrush brush = prDrawBrushImage(source, texels, PrDrawSampling_Linear);
        prDrawListRect(list, texels, {.brush = &brush});
        PrLayoutContent content{};
        content.kind = PrLayoutContentKind_DrawList;
        content.list = list;
        content.revision = revision;
        prLayoutTreeFrameSetContent(tree, node, &content);
        prDrawListRelease(list);
    }
    prKitPlace(kit, node, style.place);
    return node;
}
