// Studio Kit's icons: Lucide's glyphs, drawn as paths at any size.

#include "studio/kit/src/kit_icon.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/draw_list.h"
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

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = box.height = prLayoutLengthPx(size);
    }
};
PR_NO_PADDING_END

// What an icon's box draws, which the tree holds a reference to while it uses it.
struct KitPainted {
    Atomic<uint32_t> references;
    const PrPath* path;
    PrColor color;
    bool stroked;
};

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

void retainPainted(void* PR_NULLABLE context) {
    static_cast<KitPainted*>(context)->references.fetchAddRelaxed(1);
}

void releasePainted(void* PR_NULLABLE context) {
    KitPainted* painted = static_cast<KitPainted*>(context);
    if (painted->references.fetchSub(1) == 1)
        prMemoryFree(painted);
}

// Makes a box that draws a path in a color, in a square of `size` px, or 16 px for zero. A node keeps what it drew
// until that changes, so the box keeps the same content from round to round.
PrLayoutNode iconBox(PrKit& kit, PrLayoutNode parent, uint64_t key, const PrPath* path, PrColor color, bool stroked,
        float size) {
    size = size > 0 ? size : 16;
    PrLayoutTree* tree = kit.tree;
    PrLayoutNode node = prLayoutTreeFrameReplaced(tree, parent, key, kitStyle(kit, IconBox{.size = size}, KitFixed{}));
    PrLayoutNaturalSize natural{.width = prLayoutUnitFromPx(size), .height = prLayoutUnitFromPx(size), .ratio = 1};
    prLayoutTreeFrameSetNaturalSize(tree, node, natural);
    PrLayoutContent was = prLayoutNodeContent(tree, node);
    if (was.kind == PrLayoutContentKind_Painter && was.painter.paint == paintIcon) {
        const KitPainted* drawn = static_cast<const KitPainted*>(was.painter.context);
        if (drawn->path == path && drawn->stroked == stroked && memEquals(&drawn->color, &color, sizeof color))
            return node;
    }
    KitPainted* made = allocZeroed<KitPainted>();
    made->references.store(1);
    made->path = path;
    made->color = color;
    made->stroked = stroked;
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Painter;
    content.painter = PrLayoutPainter{paintIcon, made, retainPainted, releasePainted};
    prLayoutTreeFrameSetContent(tree, node, &content);
    releasePainted(made);
    return node;
}

} // namespace

PrLayoutNode kitIcon(PrKit& kit, PrLayoutNode parent, uint64_t key, PrKitIconKind icon, float size, PrColor color) {
    PR_ASSERT(icon < PrKitIconKind_Count);
    PrPath*& path = kit.icons[icon];
    if (path == nullptr) {
        path = prPathCreate();
        prPathAppendData(path, kKitIconData[icon].path);
    }
    return iconBox(kit, parent, key, path, color, !kKitIconData[icon].filled, size);
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
    return iconBox(*kit, parent, icon.key, icon.path, icon.color, icon.stroked, style.size);
}
