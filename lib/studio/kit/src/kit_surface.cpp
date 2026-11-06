// Studio Kit's surfaces: the backgrounds that the application, its panels, bars and cards are drawn on.

#include "pr/kit_surface.h"

#include "pr/layout_frame.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

PrColor backgroundColor(const PrKit& kit, PrKitBackground background) {
    const PrKitPalette& p = kit.theme.palette;
    switch (background) {
        case PrKitBackground_App:
            return p.bg0;
        case PrKitBackground_Panel:
            return p.bg1;
        case PrKitBackground_Header:
            return p.bg2;
        case PrKitBackground_Raised:
            return p.bg3;
        case PrKitBackground_Select:
            return p.accentWeak;
        case PrKitBackground_Viewport:
            return p.bgViewport;
        case PrKitBackground_Hud:
            return p.hudFill;
        default:
            return PrColor{0, 0, 0, 0};
    }
}

// A surface's box: its box's style, with a one px border on the sides it rules.
PR_NO_PADDING_BEGIN
struct SurfaceBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitBoxStyle style;
    PrKitRuleFlags rules;
    uint8_t _pad[3];

    void make(const PrKit& kit, PrLayoutBoxStyle& box) const {
        KitContainerBox{.style = style}.make(kit, box);
        static const PrKitRuleFlags kRules[PrRectSide_Count] = {
            PrKitRuleFlag_Top, PrKitRuleFlag_Right,
            PrKitRuleFlag_Bottom, PrKitRuleFlag_Left,
        };
        for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
            if ((rules & kRules[side]) != 0)
                box.border[side] = prLayoutUnitFromPx(1);
        }
    }
};
PR_NO_PADDING_END

// Returns a surface's background and rule colors. Rules on the HUD background use the HUD's border color.
KitColors surfaceColors(const PrKit& kit, PrKitBackground background) {
    const PrKitPalette& p = kit.theme.palette;
    return KitColors{
        .background = backgroundColor(kit, background),
        .border = background == PrKitBackground_Hud ? p.hudBorder : p.line1,
    };
}

// Returns the record of a surface in `style`.
PrLayoutStyleIndex surfaceStyle(PrKit& kit, const PrKitSurfaceStyle& style) {
    return kitStyle(kit, SurfaceBox{.style = style.box, .rules = style.rules}, kitContainerFlex(style.box),
        surfaceColors(kit, style.background));
}

// A panel header's bar: a row on the header background with a rule below it, a medium control's height.
PrKitSurfaceStyle headerBar() {
    return {
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .gap = kPrKitSpace3,
            .padding = {0, kPrKitSpace4, 0, kPrKitSpace4},
            .place = {.height = prKitPx(kPrKitHeightMd), .flex = PrKitFlex_Fixed},
        },
        .background = PrKitBackground_Header,
        .rules = PrKitRuleFlag_Bottom,
    };
}

constexpr PrKitTextStyle kHeaderTitle = {
    .role = PrKitTextRole_Caption,
    .tone = PrKitTone_Secondary,
    .wrap = PrKitWrap_Clip,
};
constexpr PrKitTextStyle kHeaderMeta = {.role = PrKitTextRole_Meta};

// A swatch's box: a bordered square of `size` px.
PR_NO_PADDING_BEGIN
struct SwatchBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = box.height = prLayoutLengthPx(size);
        kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitSurface(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitSurfaceStyle style) {
    return kitBoxNode(*kit, parent, box, style.box.overflow, surfaceStyle(*kit, style));
}

PrLayoutNode prKitSelectable(PrKit* kit, PrLayoutNode parent, PrKitSelectable selectable, PrKitBoxStyle style) {
    PrKit& k = *kit;
    PrLayoutNode node = kitBoxNode(k, parent, PrKitBox{.key = selectable.key}, style.overflow, 0);
    KitControl c = kitControlFromNode(k, node, prUiFlags(k.ui, node) | PrUiFlag_Pressable | PrUiFlag_Focusable,
        selectable.disabled);
    PrKitLookId look = selectable.selected ? PrKitLookId_SelectableSelected : PrKitLookId_Selectable;
    PrLayoutStyleIndex record =
        kitStyle(k, KitContainerBox{.style = style}, kitContainerFlex(style), kitDress(k, c, look).fill);
    prLayoutTreeFrameSetStyle(k.tree, node, record);
    return node;
}

PrLayoutNode prKitPanelHeader(PrKit* kit, PrLayoutNode parent, PrKitPanelHeader header) {
    PrKit& k = *kit;
    PrLayoutNode bar =
        kitBoxNode(k, parent, PrKitBox{.key = header.key}, PrKitOverflow_Visible, surfaceStyle(k, headerBar()));
    prKitLabel(kit, bar, PrKitLabel{.text = header.title}, kHeaderTitle);
    prKitSpacer(kit, bar, PrKitSpacer{});
    if (header.meta.size != 0)
        prKitLabel(kit, bar, PrKitLabel{.text = header.meta}, kHeaderMeta);
    return bar;
}

PrLayoutNode prKitSwatch(PrKit* kit, PrLayoutNode parent, PrKitSwatch swatch, PrKitSwatchStyle style) {
    PrKit& k = *kit;
    PrLayoutStyleIndex record = kitStyle(k, SwatchBox{.size = style.size}, KitFixed{},
        KitColors{.background = swatch.color, .border = k.theme.palette.line2});
    return prLayoutTreeFrameElement(k.tree, parent, swatch.key, record);
}
