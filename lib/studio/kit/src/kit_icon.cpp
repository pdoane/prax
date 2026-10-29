// Studio Kit's icons: Lucide's glyphs, drawn as paths at any size.

#include "studio/kit/src/kit_icon.h"

#include "pr/alloc.h"
#include "pr/draw_list.h"
#include "pr/hash.h"
#include "pr/layout_unit.h"
#include "pr/mem.h"
#include "pr/path_data.h"

#include "studio/kit/src/kit.h"
#include "studio/kit/src/kit_icon_data.h"

namespace pr_impl {

namespace {

// The grid that the set's glyphs are drawn on, and the width of their strokes on that grid.
constexpr float kGrid = 24;
constexpr float kStrokeWidth = 2;

// An icon's box: a square of `size` px.
PR_NO_PADDING_BEGIN
struct IconBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float size;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = box.height = prLayoutLengthPx(size);
    }
};
PR_NO_PADDING_END

// Draws an icon's path over its content box, scaled from the grid.
void paintIcon(void* PR_NULLABLE context, PrVec2 size, PrDrawList* list) {
    const KitPainted* painted = static_cast<const KitPainted*>(context);
    prDrawListPushTransformAffine2(list, prAffine2Scale(size / kGrid));
    if (painted->stroked) {
        PrDrawStroke stroke = prDrawStrokeDefault();
        stroke.shape.width = kStrokeWidth;
        stroke.shape.join = PrPathJoin_Round;
        stroke.shape.startCap = PrPathCap_Round;
        stroke.shape.endCap = PrPathCap_Round;
        prDrawListPath(list, painted->path, PrPathFillRule_NonZero, {.color = painted->color, .stroke = &stroke});
    } else {
        prDrawListPath(list, painted->path, PrPathFillRule_NonZero, {.color = painted->color});
    }
    prDrawListPopTransform(list);
}

// Returns the painter context of a path drawn in a color. The context is made on first use and kept while the kit
// lives, so that the node that draws it keeps the same content from round to round.
const KitPainted* paintedOf(Kit& kit, const PrPath* path, PrColor color, bool stroked) {
    KitPainted key{.path = path, .color = color, .stroked = stroked};
    uint64_t pointer = uint64_t(reinterpret_cast<uintptr_t>(path));
    uint32_t hash = uint32_t(hashBytes(&key.color, sizeof key.color, pointer * 2 + (stroked ? 1 : 0)));
    uint32_t at = kit.paintedIndex.find(hash, [&](uint32_t i) {
        const KitPainted* p = kit.painted.data[i];
        return p->path == path && p->stroked == stroked && memEquals(&p->color, &color, sizeof color);
    });
    if (at != kHashMissing)
        return kit.painted.data[at];
    KitPainted* made = alloc<KitPainted>();
    *made = key;
    kit.paintedIndex.insert(hash, kit.painted.count);
    kit.painted.push(made);
    return made;
}

// Makes a box that draws `painted` in a square of `size` px, or 16 px for zero.
PrLayoutNode iconBox(Kit& kit, PrLayoutNode parent, uint64_t key, const KitPainted* painted, float size) {
    size = size > 0 ? size : 16;
    PrLayoutTree* tree = kit.tree;
    PrLayoutNode node = prLayoutTreeFrameReplaced(tree, parent, key, kitStyle(kit, IconBox{.size = size}, KitFixed{}));
    PrLayoutNaturalSize natural{.width = prLayoutUnitFromPx(size), .height = prLayoutUnitFromPx(size), .ratio = 1};
    prLayoutTreeFrameSetNaturalSize(tree, node, natural);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Painter;
    content.painter.paint = paintIcon;
    content.painter.context = const_cast<KitPainted*>(painted);
    prLayoutTreeFrameSetContent(tree, node, &content);
    return node;
}

} // namespace

PrLayoutNode kitIcon(Kit& kit, PrLayoutNode parent, uint64_t key, PrKitIconKind icon, float size, PrColor color) {
    PR_ASSERT(icon < PrKitIconKind_Count);
    PrPath*& path = kit.icons[icon];
    if (path == nullptr) {
        path = prPathCreate();
        prPathAppendData(path, kKitIconData[icon].path);
    }
    return iconBox(kit, parent, key, paintedOf(kit, path, color, !kKitIconData[icon].filled), size);
}

} // namespace pr_impl

using namespace pr_impl;

PrStringView prKitIconName(PrKitIconKind icon) {
    return icon < PrKitIconKind_Count ? kKitIconData[icon].name : PrStringView{};
}

bool prKitIconFilled(PrKitIconKind icon) {
    return icon < PrKitIconKind_Count && kKitIconData[icon].filled;
}

PrLayoutNode prKitIcon(PrKit* kit, PrLayoutNode parent, PrKitIcon icon, PrKitIconStyle style) {
    return kitIcon(*kit, parent, icon.key, icon.icon, style.size, prKitToneColor(kit, style.tone));
}

PrLayoutNode prKitPathIcon(PrKit* kit, PrLayoutNode parent, PrKitPathIcon icon, PrKitIconStyle style) {
    return iconBox(*kit, parent, icon.key, paintedOf(*kit, icon.path, icon.color, icon.stroked), style.size);
}
