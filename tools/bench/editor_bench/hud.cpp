// The viewport's heads-up display: a component library of the editor's own, over lib/ui and lib/layout.

#include "tools/bench/editor_bench/hud.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_unit.h"
#include "pr/text_font.h"
#include "pr/ui_records.h"

namespace editor {

using namespace pr;

struct Hud {
    PrUi* ui;
    PrLayoutTree* tree;
    PrTextFont* PR_NULLABLE font;
};

namespace {

// The HUD's palette.
constexpr PrColor kGlass = {0.03f, 0.04f, 0.05f, 0.78f};
constexpr PrColor kEdge = {0.45f, 0.85f, 0.65f, 0.35f};
constexpr PrColor kDim = {0.42f, 0.56f, 0.50f, 1};
constexpr PrColor kLit = {0.70f, 0.96f, 0.80f, 1};
constexpr PrColor kLitFill = {0.24f, 0.62f, 0.44f, 0.40f};
constexpr PrColor kHoverFill = {0.45f, 0.85f, 0.65f, 0.14f};
constexpr PrColor kClear = {0, 0, 0, 0};

// Spacing in px: between a panel and its layer's edges, inside a panel's border, and between a panel's items.
constexpr float kInset = 10;
constexpr float kPadding = 6;
constexpr float kGap = 4;

// How long the pointer rests on an anchor before its hint shows, in seconds.
constexpr double kHintDelay = 0.4;

// Writes a border `width` px wide on every side.
void setBorder(PrLayoutBoxStyle& box, float width) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.border[side] = prLayoutUnitFromPx(width);
}

// Writes padding of `vertical` px above and below, and `horizontal` px at the sides.
void setPadding(PrLayoutBoxStyle& box, float vertical, float horizontal) {
    box.padding[PrRectSide_Top] = box.padding[PrRectSide_Bottom] = prLayoutLengthPx(vertical);
    box.padding[PrRectSide_Left] = box.padding[PrRectSide_Right] = prLayoutLengthPx(horizontal);
}

// The box of a layer, which covers the whole of its parent.
struct LayerBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Hud&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        for (uint32_t side = 0; side < PrRectSide_Count; ++side)
            box.inset[side] = prLayoutLengthPx(0);
    }
};

// The box of a panel, a bordered flex container in a corner of its layer.
PR_NO_PADDING_BEGIN
struct PanelBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    HudCorner corner;

    void make(const Hud&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.position = PrLayoutPosition_Absolute;
        bool right = corner == HudCorner_TopRight || corner == HudCorner_BottomRight;
        bool bottom = corner == HudCorner_BottomLeft || corner == HudCorner_BottomRight;
        box.inset[right ? PrRectSide_Right : PrRectSide_Left] = prLayoutLengthPx(kInset);
        box.inset[bottom ? PrRectSide_Bottom : PrRectSide_Top] = prLayoutLengthPx(kInset);
        setBorder(box, 1);
        setPadding(box, kPadding, kPadding + 2);
    }
};
PR_NO_PADDING_END

// A flex container's direction and gap. A row centers its items across it, and a column aligns them at its start.
struct Flow {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    bool row;

    void make(const Hud&, PrLayoutFlexStyle& flex) const {
        flex.direction = row ? PrLayoutFlexDirection_Row : PrLayoutFlexDirection_Column;
        flex.alignItems = row ? PrLayoutAlignment_Center : PrLayoutAlignment_FlexStart;
        flex.rowGap = flex.columnGap = prLayoutLengthPx(kGap);
    }
};

// A background color, and a border of one color on every side.
PR_NO_PADDING_BEGIN
struct Fill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    PrColor background;
    PrColor border;

    void make(const Hud&, PrLayoutDecorationStyle& decoration) const {
        decoration.background = prColor16FromColor(background);
        for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
            decoration.borderColor[side] = prColor16FromColor(border);
            decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
        }
    }
};
PR_NO_PADDING_END

// The HUD's text, on one line.
struct Type {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Font;
    void make(const Hud& hud, PrLayoutFontStyle& font) const {
        font.font = hud.font;
        font.lineHeight = prLayoutLineHeightPx(14);
        font.textWrapMode = PrLayoutTextWrapMode_Nowrap;
    }
};

// The color of text.
PR_NO_PADDING_BEGIN
struct Ink {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Color;
    PrColor color;

    void make(const Hud&, PrLayoutColorStyle& value) const {
        value.color = prColor16FromColor(color);
    }
};
PR_NO_PADDING_END

// A block of text.
struct TextBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Hud&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
    }
};

// The box of a row of text.
struct RowBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Hud&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
    }
};

// The box of a switch, a bordered pill around its label.
struct SwitchBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Hud&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        setBorder(box, 1);
        setPadding(box, 1, 6);
    }
};

// The box of a hint at `left` and `top` in the overlay.
PR_NO_PADDING_BEGIN
struct HintBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float left;
    float top;

    void make(const Hud&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPx(left);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
        setBorder(box, 1);
        setPadding(box, 3, 6);
    }
};
PR_NO_PADDING_END

// Creates a block of `text` in `color` under `parent`.
void textBlock(Hud& hud, PrLayoutNode parent, uint64_t key, PrStringView text, PrColor color) {
    PrLayoutStyleIndex style = uiStyle(hud.ui, hud, TextBox{}, Type{}, Ink{.color = color});
    PrLayoutNode node = prLayoutTreeFrameElement(hud.tree, parent, key, style);
    prLayoutTreeFrameText(hud.tree, node, 0, text);
}

// Returns a node's border box in viewport px at the last layout.
PrRect boundsOf(const Hud& hud, PrLayoutNode node) {
    return prLayoutRectToPx(prLayoutNodeBounds(hud.tree, node, PrLayoutCoordinates_Viewport));
}

} // namespace

Hud* hudCreate(PrUi* ui, PrFontSet* fonts) {
    Hud* hud = allocZeroed<Hud>();
    hud->ui = ui;
    hud->tree = prUiTree(ui);
    PrFontFamily family = prFontFamilyGeneric(PrFontGeneric_Monospace);
    PrFontStyle style = prFontStyleDefault();
    style.size = 11;
    PrTextFontDesc desc{.set = fonts, .families = &family, .familyCount = 1, .style = style};
    hud->font = prTextFontCreate(&desc);
    return hud;
}

void hudDestroy(Hud* PR_NULLABLE hud) {
    if (hud == nullptr)
        return;
    prUiForget(hud->ui, hud);
    prTextFontRelease(hud->font);
    prMemoryFree(hud);
}

PrLayoutNode hudLayer(Hud* hud, PrLayoutNode parent, HudLayer layer) {
    return prUiElement(hud->ui, parent, layer.key, uiStyle(hud->ui, *hud, LayerBox{}), PrUiFlag_Transparent);
}

PrLayoutNode hudPanel(Hud* hud, PrLayoutNode layer, HudPanel panel) {
    PR_ASSERT(prLayoutTreeFrameParent(hud->tree, layer) != kPrLayoutNodeNone);
    PrLayoutStyleIndex style =
        uiStyle(hud->ui, *hud, PanelBox{.corner = panel.corner}, Flow{.row = panel.row}, Fill{kGlass, kEdge});
    return prLayoutTreeFrameElement(hud->tree, layer, panel.key, style);
}

PrLayoutNode hudReadout(Hud* hud, PrLayoutNode parent, HudReadout readout) {
    PrLayoutNode row = prLayoutTreeFrameElement(hud->tree, parent, readout.key,
                                                uiStyle(hud->ui, *hud, RowBox{}, Flow{.row = true}));
    textBlock(*hud, row, 0, readout.label, kDim);
    textBlock(*hud, row, 0, readout.value, kLit);
    return row;
}

PrLayoutNode hudSwitch(Hud* hud, PrLayoutNode parent, HudSwitch control) {
    PR_ASSERT(control.value != nullptr);
    PrUi* ui = hud->ui;
    PrLayoutNode node = prUiElement(ui, parent, control.key, 0, PrUiFlag_Pressable | PrUiFlag_Focusable);
    if (prUiActivated(ui, node)) {
        prUiEdit(ui, node, PrUiEditPhase_Begin);
        *control.value = !*control.value;
        prUiEdit(ui, node, PrUiEditPhase_Change);
        prUiEdit(ui, node, PrUiEditPhase_Commit);
    }
    bool on = *control.value;
    bool ring = prUiIsFocusVisible(ui) && prUiFocus(ui) == node;
    PrColor background = on ? kLitFill : prUiIsHovered(ui, node) ? kHoverFill : kClear;
    prLayoutTreeFrameSetStyle(hud->tree, node,
                              uiStyle(ui, *hud, SwitchBox{}, Type{}, Ink{.color = on ? kLit : kDim},
                                      Fill{background, ring ? kLit : kEdge}));
    prUiSetCursor(ui, node, PrAppCursor_Hand);
    prLayoutTreeFrameText(hud->tree, node, 0, control.label);
    return node;
}

void hudHint(Hud* hud, PrLayoutNode anchor, PrStringView text) {
    PrUi* ui = hud->ui;
    double rested = prUiHoverSeconds(ui, anchor);
    if (rested <= 0)
        return;
    if (rested < kHintDelay) {
        prUiRequestFrame(ui);
        return;
    }
    PrLayoutNode overlay = prUiOverlay(ui);
    PrRect at = boundsOf(*hud, anchor);
    PrRect origin = boundsOf(*hud, overlay);
    HintBox box{.left = at.left - origin.left, .top = at.bottom - origin.top + kGap};
    PrLayoutStyleIndex style = uiStyle(ui, *hud, box, Type{}, Ink{.color = kLit}, Fill{kGlass, kEdge});
    PrLayoutNode hint = prUiElement(ui, overlay, prUiKey(ui, anchor, "hud hint"), style, PrUiFlag_SubtreeTransparent);
    prLayoutTreeFrameText(hud->tree, hint, 0, text);
}

} // namespace editor
