// Studio Kit's icons: the glyphs of Studio's set (pr/studio_icon.h), drawn as paths at any size.

#include "studio/kit/src/kit_icon.h"

#include "pr/layout_unit.h"
#include "pr/path_data.h"
#include "pr/studio_icon.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// An icon's box: a square of `size` px.
PR_NO_PADDING_BEGIN
struct IconBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = box.height = prLayoutLengthPx(size);
    }
};
PR_NO_PADDING_END

// Makes a box that draws a path in a color, in a square of `size` px, or 16 px for zero, on the set's grid. The tree
// draws its clone of the path, which keeps the path's id, so the same path draws from what the renderer derived from
// it.
PrLayoutNode iconBox(PrKit& kit, PrLayoutNode parent, uint64_t key, const PrPath* path, PrColor color, bool stroked,
        float size) {
    size = size > 0 ? size : 16;
    PrLayoutTree* tree = kit.tree;
    PrLayoutNode node = prLayoutTreeFrameReplaced(tree, parent, key, kitStyle(kit, IconBox{.size = size}, KitFixed{}));
    PrLayoutNaturalSize natural{.width = prLayoutUnitFromPx(size), .height = prLayoutUnitFromPx(size), .ratio = 1};
    prLayoutTreeFrameSetNaturalSize(tree, node, natural);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Path;
    content.path = PrLayoutPath{
        .path = path,
        .viewBox = PrVec2{kPrStudioIconGrid, kPrStudioIconGrid},
        .color = prColor16FromColor(color),
        .strokeWidth = stroked ? kPrStudioIconStrokeWidth : 0,
        .rule = PrPathFillRule_NonZero,
        .join = PrPathJoin_Round,
        .cap = PrPathCap_Round,
    };
    prLayoutTreeFrameSetContent(tree, node, &content);
    return node;
}

} // namespace

const PrPath* kitIconPath(PrKit& kit, PrStudioIconKind icon) {
    PR_ASSERT(icon < PrStudioIconKind_Count);
    const PrPath*& path = kit.icons[icon];
    if (path != nullptr)
        return path;
    PrPathCache* cache = kit.theme.paths;
    if (cache == nullptr) {
        if (kit.paths == nullptr)
            kit.paths = prPathCacheCreate();
        cache = kit.paths;
    }
    path = prPathCacheFind(cache, prStudioIconPathData(icon));
    return path;
}

PrLayoutNode kitIcon(PrKit& kit, PrLayoutNode parent, uint64_t key, PrStudioIconKind icon, float size, PrColor color) {
    return iconBox(kit, parent, key, kitIconPath(kit, icon), color, !prStudioIconIsFilled(icon), size);
}

} // namespace pr_impl

using namespace pr_impl;

const PrPath* prKitIconPath(PrKit* kit, PrStudioIconKind icon) {
    return kitIconPath(*kit, icon);
}

PrLayoutNode prKitIcon(PrKit* kit, PrLayoutNode parent, PrKitIcon icon, PrKitIconStyle style) {
    return kitIcon(*kit, parent, icon.key, icon.icon, style.size, prKitToneColor(kit, style.tone));
}

PrLayoutNode prKitPathIcon(PrKit* kit, PrLayoutNode parent, PrKitPathIcon icon, PrKitIconStyle style) {
    return iconBox(*kit, parent, icon.key, icon.path, icon.color, icon.stroked, style.size);
}
