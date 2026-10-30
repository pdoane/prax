// Studio Kit's boxes: containers that lay their children out in a row or a column, and the space between them.

#include "pr/kit_box.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// How a box is positioned: in flow, as a stack that its layers cover, or as a layer over its stack.
enum : uint8_t {
    Positioning_Flow,
    Positioning_Stack,
    Positioning_Layer,
};
typedef uint8_t Positioning;

// A box's box group: its style's, positioned.
PR_NO_PADDING_BEGIN
struct BoxBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitBoxStyle style;
    Positioning positioning;
    uint8_t _pad[3];

    void make(const Kit& kit, PrLayoutBoxStyle& box) const {
        KitContainerBox{.style = style}.make(kit, box);
        if (positioning == Positioning_Stack) {
            box.position = PrLayoutPosition_Relative;
        } else if (positioning == Positioning_Layer) {
            box.position = PrLayoutPosition_Absolute;
            for (uint32_t side = 0; side < PrRectSide_Count; ++side)
                box.inset[side] = prLayoutLengthPx(0);
        }
    }
};
PR_NO_PADDING_END

PrLayoutNode positioned(Kit& kit, PrLayoutNode parent, PrKitBox box, const PrKitBoxStyle& style,
        Positioning positioning) {
    PrLayoutStyleIndex record =
        kitStyle(kit, BoxBox{.style = style, .positioning = positioning}, kitContainerFlex(style));
    return kitBoxNode(kit, parent, box, style.overflow, record);
}

// A plane's box group: its style's, transformed, and at a stack level if it has one.
PR_NO_PADDING_BEGIN
struct PlaneBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitBoxStyle style;
    bool zIndexed;
    uint8_t _pad[3];

    void make(const Kit& kit, PrLayoutBoxStyle& box) const {
        KitContainerBox{.style = style}.make(kit, box);
        box.transformed = 1;
        box.zIndexed = zIndexed ? 1 : 0;
    }
};
PR_NO_PADDING_END

// A plane's effects: its perspective, level and back face, and its transform, which is a value that moves.
PR_NO_PADDING_BEGIN
struct PlaneEffects {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    PrMatrix4 transform;
    float perspective;
    int32_t zIndex;
    bool transformed;
    bool backfaceHidden;
    uint8_t _pad[2];

    void make(const Kit&, PrLayoutEffectStyle& effects) const {
        if (transformed)
            effects.transform = PrLayoutTransform{.matrix = transform};
        if (perspective > 0)
            effects.perspective = perspective > 1 ? perspective : 1;
        effects.zIndex = zIndex;
        effects.backfaceHidden = backfaceHidden;
    }
};
PR_NO_PADDING_END

// A divider's groups: a rule one px thick across its parent. It has its own size in both axes, and its parent's
// alignment stretches it across.
struct DividerBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.minWidth = prLayoutLengthPx(1);
        box.minHeight = prLayoutLengthPx(1);
    }
};
struct DividerFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.shrink = 0;
        flex.alignSelf = PrLayoutAlignment_Stretch;
    }
};
struct DividerFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    void make(const Kit& kit, PrLayoutDecorationStyle& decoration) const {
        decoration.background = kitColor16(kit.theme.palette.line1);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitBox(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitBoxStyle style) {
    return positioned(*kit, parent, box, style, Positioning_Flow);
}

PrLayoutNode prKitStack(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitBoxStyle style) {
    return positioned(*kit, parent, box, style, Positioning_Stack);
}

PrLayoutNode prKitLayer(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitBoxStyle style) {
    return positioned(*kit, parent, box, style, Positioning_Layer);
}

PrLayoutNode prKitPlane(PrKit* kit, PrLayoutNode parent, PrKitPlane plane, PrKitPlaneStyle style) {
    PlaneEffects effects{
        .transform = plane.transform != nullptr ? *plane.transform : PrMatrix4{},
        .perspective = style.perspective,
        .zIndex = style.zIndex,
        .transformed = plane.transform != nullptr,
        .backfaceHidden = style.backfaceHidden,
    };
    PrLayoutStyleIndex record = kitStyle(*kit, PlaneBox{.style = style.box, .zIndexed = style.zIndex != 0},
        kitContainerFlex(style.box), effects);
    return kitBoxNode(*kit, parent, PrKitBox{.key = plane.key, .disabled = plane.disabled}, style.box.overflow, record);
}

PrLayoutNode prKitSpacer(PrKit* kit, PrLayoutNode parent, PrKitSpacer spacer) {
    return prLayoutTreeFrameElement(kit->tree, parent, spacer.key, kitStyle(*kit, KitShrinkable{}, KitGrow{}));
}

PrLayoutNode prKitDivider(PrKit* kit, PrLayoutNode parent, PrKitDivider divider) {
    return prLayoutTreeFrameElement(kit->tree, parent, divider.key,
        kitStyle(*kit, DividerBox{}, DividerFlex{}, DividerFill{}));
}
