// Studio Kit's menus, popovers, the menu bar, context menus, tooltips, modals and the command palette.

#include "pr/kit_menu.h"

#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/layout_query.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// The keys of this file's retained state.
enum : uint64_t {
    MenuStateKey_Open = KitStateKey_Menu,   // OpenState, on a trigger
    MenuStateKey_Context,                   // ContextState, on a context menu's host
    MenuStateKey_Bar,                       // BarState, on a menu bar
    MenuStateKey_Palette,                   // PaletteState, on a palette's frame
    MenuStateKey_Select,                    // SelectState, on a select's field
};
typedef uint64_t MenuStateKey;

// The kinds of floating node, which distinguish the keys of the nodes that float from one node.
enum : uint64_t {
    FloatKind_Popover = 1,
    FloatKind_Context,
    FloatKind_Tooltip,
    FloatKind_Modal,
    FloatKind_Ghost,
};
typedef uint64_t FloatKind;

// The scrim's color, the same in every theme.
constexpr PrColor kScrim = {0, 0, 0, 0.55f};

// The defaults, in px, of a modal's top inset, a dialog's width, a palette's width and the height of a palette's list.
constexpr float kModalInset = 120;
constexpr float kDialogWidth = 420;
constexpr float kPaletteWidth = 560;
constexpr float kPaletteListHeight = 320;

// The size in px of the icons in menu items, and of the check column.
constexpr float kItemIcon = 12;

// The greatest height in px of a select's list, past which it scrolls.
constexpr float kSelectListHeight = 320;

// The open state of a trigger, and the floating node that it showed in the last round in which it was open.
struct OpenState {
    PrLayoutNode surface;
    bool open;
};

// The open state of a host's context menu, where the menu opens, and its floating node in the last round in which it
// was open.
struct ContextState {
    PrLayoutNode surface;
    PrVec2 point;
    bool open;
};

// The context that a floating surface provides to what is created in it, so that an item's activation can close the
// surface. It names the node that holds the surface's open state, and the key of that state.
struct Closer {
    PrLayoutNode node;
    uint64_t key;
};

// The context that a command palette provides to its list for the current round. It holds the palette's closer and
// card, the selected item's index, and the number of items created so far. `entered` is true if Enter activates the
// selected item in the round, and `moved` is true if the selection moved in the round.
struct Palette {
    Closer closer;
    PrLayoutNode frame;
    uint32_t selected;
    uint32_t count;
    bool entered;
    bool moved;
};

// The context that a select provides to its menu: the select's field, and its retained flag that is true once the open
// menu has scrolled the chosen option into view.
struct SelectMenu {
    PrLayoutNode field;
    bool* revealed;
};

// A menu bar's open item at the last round, and its items, in order.
struct BarState {
    PrLayoutNode open;
    uint32_t count;
    PrLayoutNode items[kPrKitMenuBarMaxItems];
};

// A command palette's selected item.
struct PaletteState {
    uint32_t selected;
};

// Whether a select's open list has scrolled its chosen item into view since it opened.
struct SelectState {
    bool revealed;
};

OpenState& openOf(Kit& kit, PrLayoutNode trigger) {
    PR_ASSERT(trigger != kPrLayoutNodeNone);
    return kitState<OpenState>(kit, trigger, MenuStateKey_Open);
}

ContextState& contextOf(Kit& kit, PrLayoutNode host) {
    PR_ASSERT(host != kPrLayoutNodeNone);
    return kitState<ContextState>(kit, host, MenuStateKey_Context);
}

// Returns the closer of the floating surface that `node` is in, or null if it is in none.
const Closer* PR_NULLABLE closerOf(Kit& kit, PrLayoutNode node) {
    return kitContext<Closer>(kit, node, KitContext_Closer);
}

// Closes the floating widget that `closer` names, if any, and requests the frame that removes it.
void close(Kit& kit, const Closer* PR_NULLABLE closer) {
    if (closer == nullptr || closer->node == kPrLayoutNodeNone)
        return;
    if (closer->key == MenuStateKey_Context)
        contextOf(kit, closer->node).open = false;
    else
        openOf(kit, closer->node).open = false;
    prUiRequestFrame(kit.ui);
}

// An item that itemNode created, and whether it is disabled.
struct Item {
    PrLayoutNode node;
    bool disabled;
};

// Creates an item under `parent` that takes `flags`. The item is disabled if `disabled` is true, or if a container that
// it is created under is. An enabled item shows the hand cursor.
Item itemNode(Kit& kit, PrLayoutNode parent, uint64_t key, PrUiFlags flags, bool disabled) {
    PrLayoutNode node = prLayoutTreeFrameElement(kit.tree, parent, key, 0);
    bool off = kitSetFlags(kit, node, disabled ? flags | PrUiFlag_Disabled : flags);
    if (!off)
        prUiSetCursor(kit.ui, node, PrAppCursor_Hand);
    return Item{.node = node, .disabled = off};
}

// Returns the key of a node that floats from `from`. The key is unique in the tree.
uint64_t floatKey(PrLayoutNode from, uint64_t kind, uint64_t key) {
    return hashMixKeys(hashMixKeys(from, kind), key);
}

PrRect boundsOf(Kit& kit, PrLayoutNode node) {
    return prLayoutRectToPx(prLayoutNodeBounds(kit.tree, node, PrLayoutCoordinates_Viewport));
}

// Returns true if the node had a box in the last layout.
bool laidOut(Kit& kit, PrLayoutNode node) {
    PrRect r = boundsOf(kit, node);
    return r.right > r.left || r.bottom > r.top;
}

// Returns true if the round dismisses a floating node shown in the last round: by Escape, claimed on the node or on
// `from`, or by a primary press outside both. Claims Escape on both for the next round.
bool dismissed(Kit& kit, PrLayoutNode surface, PrLayoutNode from) {
    bool escape = prUiKeyPressed(kit.ui, from, PrAppKey_Escape, 0);
    if (surface == kPrLayoutNodeNone)
        return escape;
    escape = prUiKeyPressed(kit.ui, surface, PrAppKey_Escape, 0) || escape;
    return escape || (prUiPressedOutside(kit.ui, surface, 0) && prUiPressedOutside(kit.ui, from, 0));
}

// Returns the insets from the overlay's corner that place a box of `extent` at `offset` from the bottom left corner of
// `anchor`, inside `bounds`. The box goes below the anchor if it fits, and above it otherwise, and moves left if it
// runs past the right edge. An extent of zero, before the box has been laid out, is placed as given. An empty `bounds`
// places the box with no limit.
PrVec2 placeFloating(PrRect bounds, PrRect anchor, PrVec2 extent, PrVec2 offset) {
    float x = anchor.left + offset.x;
    float y = anchor.bottom + offset.y;
    bool bounded = bounds.right > bounds.left && bounds.bottom > bounds.top;
    if (!bounded)
        return PrVec2{x, y};
    if (x + extent.x > bounds.right)
        x = bounds.right - extent.x;
    if (y + extent.y > bounds.bottom) {
        float above = anchor.top - offset.y - extent.y;
        y = above >= bounds.top ? above : bounds.bottom - extent.y;
    }
    x = x < bounds.left ? bounds.left : x;
    y = y < bounds.top ? bounds.top : y;
    return PrVec2{x - bounds.left, y - bounds.top};
}

// A row of `height` px, or of its content's height for 0, padded by `padding`, with a one px rule on the sides that
// `rules` names, a one px border on every side if `bordered`, and clipping its content, which lets its parent shrink
// it, if `clips`.
PR_NO_PADDING_BEGIN
struct RowBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float height;
    PrKitEdges padding;
    PrKitRuleFlags rules;
    bool bordered;
    bool clips;
    uint8_t _pad;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        if (height > 0)
            box.height = prLayoutLengthPx(height);
        kitSetPadding(box, padding);
        if (bordered)
            kitSetBorderWidth(box, 1);
        static const PrKitRuleFlags kRules[PrRectSide_Count] = {
            PrKitRuleFlag_Top, PrKitRuleFlag_Right,
            PrKitRuleFlag_Bottom, PrKitRuleFlag_Left,
        };
        for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
            if ((rules & kRules[side]) != 0)
                box.border[side] = prLayoutUnitFromPx(1);
        }
        if (clips) {
            box.minWidth = prLayoutLengthPx(0);
            box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
        }
    }
};

// A row's flex: its children centered across it, apart by `gap` px, and the row never shrinks.
struct RowFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    float gap;

    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(gap);
        flex.shrink = 0;
    }
};

// A column that stretches its children across it, and never shrinks if `fixed`.
struct ColumnFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    bool fixed;

    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
        flex.alignItems = PrLayoutAlignment_Stretch;
        if (fixed)
            flex.shrink = 0;
    }
};
PR_NO_PADDING_END

// What a floating node looks like: a popover's or a context menu's raised column, a tooltip, or a drag's ghost.
enum : uint8_t {
    FloatLook_Surface,
    FloatLook_Tooltip,
    FloatLook_Ghost,
};
typedef uint8_t FloatLook;

// A floating node's box: its look's, at least `minWidth` px wide if that is nonzero, placed at `left` and `top` in the
// overlay. The place moves with what the node floats against, and only this group with it.
PR_NO_PADDING_BEGIN
struct FloatBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    FloatLook look;
    uint8_t _pad[3];
    float minWidth;
    float left;
    float top;

    void make(const Kit& kit, PrLayoutBoxStyle& box) const {
        if (look == FloatLook_Tooltip)
            RowBox{.padding = prKitEdgesXY(kPrKitSpace1, kPrKitSpace3), .bordered = true}.make(kit, box);
        else if (look == FloatLook_Ghost)
            RowBox{.height = kPrKitHeightSm, .padding = prKitEdgesXY(0, kPrKitSpace3), .bordered = true}.make(kit, box);
        else
            box.display = PrLayoutDisplay_Flex;
        if (look == FloatLook_Surface)
            kitSetBorderWidth(box, 1);
        if (minWidth > 0)
            box.minWidth = prLayoutLengthPx(minWidth);
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPx(left);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(top);
    }
};

// A ghost is drawn a little translucent, over what it is dragged across.
struct FloatEffects {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    float opacity;

    void make(const Kit&, PrLayoutEffectStyle& effects) const {
        effects.opacity = opacity;
    }
};
PR_NO_PADDING_END

// Creates a node under the overlay in `look`, at least `minWidth` px wide, placed against `anchor` by `offset`. A node
// that has not been laid out yet has no extent to place by, so it requests the frame that places it.
PrLayoutNode floating(Kit& kit, uint64_t key, PrUiFlags flags, PrRect anchor, PrVec2 offset, FloatLook look,
        float minWidth) {
    PrLayoutNode overlay = prUiOverlay(kit.ui);
    PrLayoutNode node = prUiElement(kit.ui, overlay, key, 0, flags);
    PrRect bounds = boundsOf(kit, node);
    PrVec2 extent{bounds.right - bounds.left, bounds.bottom - bounds.top};
    if (extent.x <= 0 && extent.y <= 0)
        prUiRequestFrame(kit.ui);
    PrVec2 at = placeFloating(boundsOf(kit, overlay), anchor, extent, offset);
    const PrKitPalette& p = kit.theme.palette;
    FloatBox box{.look = look, .minWidth = minWidth, .left = at.x, .top = at.y};
    PrLayoutStyleIndex record = 0;
    if (look == FloatLook_Surface) {
        record = kitStyle(kit, box, ColumnFlex{}, KitColors{.background = p.bg2, .border = p.line3});
    } else {
        bool ghost = look == FloatLook_Ghost;
        KitColors colors{.background = p.bg3, .border = ghost ? p.accent : p.line3};
        record = kitStyle(kit, box, RowFlex{.gap = kPrKitSpace3}, colors, FloatEffects{.opacity = ghost ? 0.9f : 1});
    }
    prLayoutTreeFrameSetStyle(kit.tree, node, record);
    return node;
}

// ---------------------------------------------------------------------------------------------------------------------
// Menus

// A menu: a column of its items, at least `minWidth` px wide if that is nonzero.
PR_NO_PADDING_BEGIN
struct MenuBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float minWidth;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        kitSetPadding(box, prKitEdgesXY(kPrKitSpace1, 0));
        if (minWidth > 0)
            box.minWidth = prLayoutLengthPx(minWidth);
    }
};
PR_NO_PADDING_END

// A menu item's check column, which holds its check mark.
struct CheckBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.width = box.height = prLayoutLengthPx(kItemIcon);
    }
};

// A menu's caption, padded for its place among the items.
struct MenuLabelBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        kitSetPadding(box, PrKitEdges{kPrKitSpace2, kPrKitSpace4, kPrKitSpace1, kPrKitSpace4});
    }
};

// A rule one px tall between a menu's items.
struct SeparatorBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.height = prLayoutLengthPx(1);
        box.margin[PrRectSide_Top] = prLayoutLengthPx(kPrKitSpace1);
        box.margin[PrRectSide_Bottom] = prLayoutLengthPx(kPrKitSpace1);
    }
};

// How a menu item or a command item is lit.
enum {
    ItemState_Rest,
    ItemState_Hover,
    ItemState_Press,
};
typedef uint8_t ItemState;

// Returns the fill of a menu item or a command item lit in `state`, which is the accent's wash, deeper while pressed or
// while the palette's selected item is hovered.
PrColor itemFill(const Kit& kit, ItemState state, bool selected) {
    const PrKitPalette& p = kit.theme.palette;
    PrColor clear{0, 0, 0, 0};
    PrColor deep = kitMix(p.accentWeak, p.accent, 0.25f);
    switch (state) {
        case ItemState_Hover:
            return selected ? deep : p.accentWeak;
        case ItemState_Press:
            return deep;
        default:
            return selected ? p.accentWeak : clear;
    }
}

// Returns the state that an item is lit in: pressed, under the pointer or with visible focus, or at rest. The wash is
// full in the round in which the pointer arrives, so that a pointer crossing a menu lights each item that it passes.
ItemState itemState(Kit& kit, PrLayoutNode node, bool disabled) {
    if (disabled)
        return ItemState_Rest;
    if (prUiIsPressed(kit.ui, node, 0))
        return ItemState_Press;
    bool focused = prUiIsFocusVisible(kit.ui) && prUiFocus(kit.ui) == node;
    return prUiIsHovered(kit.ui, node) || focused ? ItemState_Hover : ItemState_Rest;
}

// Holds the keyboard in an open menu. Moves focus onto the menu's first item once the menu has been laid out, and
// between its items with Up and Down.
void holdKeyboard(Kit& kit, PrLayoutNode menu) {
    bool down = prUiKeyPressed(kit.ui, menu, PrAppKey_Down, 0);
    bool up = prUiKeyPressed(kit.ui, menu, PrAppKey_Up, 0);
    if (!prUiIsFocusWithin(kit.ui, menu)) {
        if (laidOut(kit, menu))
            prUiFocusMove(kit.ui, menu, false);
        else
            prUiRequestFrame(kit.ui);
    } else if (down || up) {
        prUiFocusMove(kit.ui, menu, up);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Selects

// A select's field: a raised row with a border, which is the accent while its list is open, lit by its state.
PR_NO_PADDING_BEGIN
struct SelectFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    ItemState state;
    bool open;
    bool ring;                      // shows the focus ring
    bool disabled;

    void make(const Kit& kit, PrLayoutDecorationStyle& decoration) const {
        const PrKitPalette& p = kit.theme.palette;
        const PrColor fills[] = {p.bg2, p.bg3, p.bg4};
        decoration.background = kitColor16(disabled ? p.bg2 : fills[state]);
        kitSetBorderColor(decoration, open ? p.accent : p.line2);
        if (ring) {
            decoration.outlineColor = kitColor16(p.accent);
            decoration.outlineStyle = PrDrawBorderStyle_Solid;
            decoration.outlineWidth = prLayoutUnitFromPx(1);
            decoration.outlineOffset = prLayoutUnitFromPx(1);
        }
    }
};
PR_NO_PADDING_END

// The box that holds a select's list: a column that scrolls past its greatest height.
struct SelectListBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.maxHeight = prLayoutLengthPx(kSelectListHeight);
        box.overflowX = PrLayoutOverflow_Hidden;
        box.overflowY = PrLayoutOverflow_Scroll;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Menu bars

// A menu bar: a row of its items, which stretch across it, with a rule below it.
struct BarBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(1);
    }
};
struct BarFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Stretch;
        flex.shrink = 0;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Modals, dialogs and palettes

// A modal's scrim: a layer over the whole overlay that centers its content, or holds it `inset` px from the top if
// `top` is set.
PR_NO_PADDING_BEGIN
struct ScrimBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float inset;
    bool top;
    uint8_t _pad[3];

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.position = PrLayoutPosition_Absolute;
        for (uint32_t side = 0; side < PrRectSide_Count; ++side)
            box.inset[side] = prLayoutLengthPx(0);
        if (top)
            box.padding[PrRectSide_Top] = prLayoutLengthPx(inset);
    }
};
struct ScrimFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    bool top;

    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = top ? PrLayoutAlignment_FlexStart : PrLayoutAlignment_Center;
    }
};

// A dialog's or a palette's card: a bordered column `width` px wide.
struct CardBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.width = prLayoutLengthPx(width);
        kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END

// A flex container of the display's default, for a part that sets nothing else in its box.
struct FlexBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
    }
};

// Returns the record of a card `width` px wide.
PrLayoutStyleIndex cardStyle(Kit& kit, float width) {
    const PrKitPalette& p = kit.theme.palette;
    return kitStyle(kit, CardBox{.width = width}, ColumnFlex{.fixed = true}, KitColors{.background = p.bg1, .border = p.line3});
}

// Returns the record of a row of a modal, ruled on the sides that `rules` names in the faintest line color.
PrLayoutStyleIndex ruledRow(Kit& kit, RowBox box, float gap, PrColor fill) {
    return kitStyle(kit, box, RowFlex{.gap = gap}, KitColors{.background = fill, .border = kit.theme.palette.line1});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prKitIsOpen(PrKit* kit, PrLayoutNode trigger) {
    return openOf(*kit, trigger).open;
}

void prKitSetOpen(PrKit* kit, PrLayoutNode trigger, bool open) {
    openOf(*kit, trigger).open = open;
}

bool prKitToggleOpen(PrKit* kit, PrLayoutNode trigger) {
    OpenState& state = openOf(*kit, trigger);
    state.open = !state.open;
    return state.open;
}

PrLayoutNode prKitPopover(PrKit* kit, PrLayoutNode trigger, PrKitPopover popover, PrKitPopoverStyle style) {
    OpenState& state = openOf(*kit, trigger);
    if (state.open && dismissed(*kit, state.surface, trigger)) {
        // Escape leaves focus where the popover was opened from, and a press puts focus where the press landed.
        if (prUiIsFocusWithin(kit->ui, state.surface))
            prUiSetFocus(kit->ui, trigger);
        state.open = false;
    }
    if (!state.open) {
        state.surface = kPrLayoutNodeNone;
        return kPrLayoutNodeNone;
    }
    PrLayoutNode node = floating(*kit, floatKey(trigger, FloatKind_Popover, popover.key), 0, boundsOf(*kit, trigger),
        style.offset, FloatLook_Surface, style.minWidth);
    if (state.surface != node)
        prUiKeyPressed(kit->ui, node, PrAppKey_Escape, 0);
    state.surface = node;
    kitProvide<Closer>(*kit, node, KitContext_Closer) = Closer{.node = trigger, .key = MenuStateKey_Open};
    return node;
}

PrLayoutNode prKitMenu(PrKit* kit, PrLayoutNode parent, PrKitMenu menu, PrKitMenuStyle style) {
    PrLayoutStyleIndex record = kitStyle(*kit, MenuBox{.minWidth = style.minWidth}, ColumnFlex{.fixed = true});
    PrLayoutNode node = prUiElement(kit->ui, parent, menu.key, record, PrUiFlag_FocusScope);
    // A menu in a floating surface holds the keyboard while the surface is open.
    if (closerOf(*kit, parent) != nullptr && !style.leavesFocus)
        holdKeyboard(*kit, node);
    return node;
}

PrLayoutNode prKitMenuLabel(PrKit* kit, PrLayoutNode menu, PrKitMenuLabel label) {
    PrLayoutStyleIndex record = kitStyle(*kit, MenuLabelBox{}, KitFixed{}, KitFont{.role = PrKitTextRole_Caption},
        KitInk{.color = kit->theme.palette.text4});
    PrLayoutNode node = prLayoutTreeFrameElement(kit->tree, menu, label.key, record);
    prLayoutTreeFrameText(kit->tree, node, 0, label.text);
    return node;
}

PrLayoutNode prKitMenuItem(PrKit* kit, PrLayoutNode menu, PrKitMenuItem item, PrKitMenuItemStyle style) {
    Kit& k = *kit;
    Item row = itemNode(k, menu, item.key, PrUiFlag_Pressable | PrUiFlag_Focusable, item.disabled);
    bool disabled = row.disabled;
    RowBox box{.height = kPrKitHeightMd, .padding = prKitEdgesXY(0, kPrKitSpace4)};
    KitColors fill{.background = itemFill(k, itemState(k, row.node, disabled), false)};
    prLayoutTreeFrameSetStyle(k.tree, row.node, kitStyle(k, box, RowFlex{.gap = kPrKitSpace3}, fill));

    PrKitTone tone = PrKitTone_Primary;
    if (disabled)
        tone = PrKitTone_Faint;
    else if (style.variant == PrKitMenuItemVariant_Danger)
        tone = PrKitTone_Danger;
    PrLayoutNode check = prLayoutTreeFrameElement(k.tree, row.node, 0, kitStyle(k, CheckBox{}, KitFixed{}));
    if (item.checked)
        prKitIcon(kit, check, {.icon = PrKitIconKind_Check},
            {.size = kItemIcon, .tone = PrKitTone(disabled ? PrKitTone_Faint : PrKitTone_Accent)});
    if (item.icon != PrKitIconKind_None)
        prKitIcon(kit, row.node, {.icon = item.icon}, {.size = kItemIcon, .tone = tone});
    PrKitTextStyle text{.role = PrKitTextRole_Body, .tone = tone, .wrap = PrKitWrap_Nowrap};
    prKitLabel(kit, row.node, {.text = item.text}, text);
    prKitSpacer(kit, row.node, {});
    if (item.shortcut.size != 0)
        prKitKbd(kit, row.node, {.text = item.shortcut});

    // In a select's menu, the checked option scrolls into view once the menu has been laid out, and a choice gives
    // focus back to the field, from which Up and Down step on.
    PrLayoutNode provider = kPrLayoutNodeNone;
    SelectMenu* select = kitContext<SelectMenu>(k, menu, KitContext_SelectMenu, &provider);
    if (select != nullptr && provider == menu) {
        if (item.checked && !*select->revealed && laidOut(k, row.node)) {
            prUiReveal(k.ui, row.node);
            *select->revealed = true;
        }
        if (prUiActivated(k.ui, row.node))
            prUiSetFocus(k.ui, select->field);
    }
    if (prUiActivated(k.ui, row.node))
        close(k, closerOf(k, menu));
    return row.node;
}

PrLayoutNode prKitMenuSeparator(PrKit* kit, PrLayoutNode menu, PrKitMenuSeparator separator) {
    return prLayoutTreeFrameElement(kit->tree, menu, separator.key, kitStyle(*kit, SeparatorBox{}, KitFixed{},
        KitColors{.background = kit->theme.palette.line1}));
}

PrLayoutNode prKitSelect(PrKit* kit, PrLayoutNode parent, PrKitSelect select, PrKitChoiceStyle style) {
    Kit& k = *kit;
    PrUi* ui = k.ui;
    Item field = itemNode(k, parent, select.key, PrUiFlag_Pressable | PrUiFlag_Focusable, select.disabled);
    bool disabled = field.disabled;
    if (prUiActivated(ui, field.node))
        prKitToggleOpen(kit, field.node);
    bool open = prKitIsOpen(kit, field.node);
    // Up and Down step the choice while the menu is closed.
    if (!open && !disabled) {
        if (prUiKeyPressed(ui, field.node, PrAppKey_Up, 0))
            kitSignal(k, field.node, KitSignal_StepBack);
        if (prUiKeyPressed(ui, field.node, PrAppKey_Down, 0))
            kitSignal(k, field.node, KitSignal_StepForward);
    }
    // The field sets its text's font, and clips the text before the chevron.
    RowBox box{
        .height = kitControlHeight(style.size),
        .padding = prKitEdgesXY(0, kPrKitSpace4),
        .bordered = true,
        .clips = true,
    };
    SelectFill fill{
        .state = itemState(k, field.node, disabled),
        .open = open,
        .ring = prUiIsFocusVisible(ui) && prUiFocus(ui) == field.node,
        .disabled = disabled,
    };
    bool small = style.size == PrKitControlSize_Xs || style.size == PrKitControlSize_Sm;
    KitFont font{.role = PrKitTextRole(small ? PrKitTextRole_BodySmall : PrKitTextRole_Body), .wrap = PrKitWrap_Clip};
    KitInk ink{.color = disabled ? k.theme.palette.text4 : k.theme.palette.text1};
    prLayoutTreeFrameSetStyle(k.tree, field.node, kitStyle(k, box, RowFlex{.gap = kPrKitSpace3}, fill, font, ink));
    if (select.icon != PrKitIconKind_None)
        prKitIcon(kit, field.node, {.icon = select.icon},
            {.size = kItemIcon, .tone = PrKitTone(disabled ? PrKitTone_Faint : PrKitTone_Secondary)});
    if (select.label.size != 0)
        prLayoutTreeFrameText(k.tree, field.node, 0, select.label);
    prKitSpacer(kit, field.node, {});
    prKitIcon(kit, field.node, {.icon = PrKitIconKind_ChevronDown},
        {.size = kItemIcon, .tone = PrKitTone(disabled ? PrKitTone_Faint : PrKitTone_Muted)});
    return field.node;
}

PrLayoutNode prKitSelectMenu(PrKit* kit, PrLayoutNode select) {
    Kit& k = *kit;
    PrRect bounds = boundsOf(k, select);
    PrLayoutNode popover =
        prKitPopover(kit, select, {}, {.offset = PrVec2{0, 2}, .minWidth = bounds.right - bounds.left});
    SelectState& state = kitState<SelectState>(k, select, MenuStateKey_Select);
    if (popover == kPrLayoutNodeNone) {
        state.revealed = false;
        return kPrLayoutNodeNone;
    }
    PrLayoutNode list = prLayoutTreeFrameElement(k.tree, popover, 0, kitStyle(k, SelectListBox{}, ColumnFlex{}));
    PrLayoutNode menu = prKitMenu(kit, list, {}, {});
    kitProvide<SelectMenu>(k, menu, KitContext_SelectMenu) = SelectMenu{.field = select, .revealed = &state.revealed};
    return menu;
}

bool prKitStepped(const PrKit* kit, PrLayoutNode select, int32_t* step) {
    if (kitSignaled(*kit, select, KitSignal_StepBack)) {
        *step = -1;
        return true;
    }
    if (kitSignaled(*kit, select, KitSignal_StepForward)) {
        *step = 1;
        return true;
    }
    return false;
}

PrLayoutNode prKitMenuBar(PrKit* kit, PrLayoutNode parent, PrKitMenuBar bar) {
    const PrKitPalette& p = kit->theme.palette;
    PrLayoutNode node = prLayoutTreeFrameElement(kit->tree, parent, bar.key,
        kitStyle(*kit, BarBox{}, BarFlex{}, KitColors{.background = p.bg2, .border = p.line1}));
    // The open menu passes to the item under the pointer before any item is created, so that only one menu is open in
    // the round. The items are those of the last round, whose handles the round's items continue.
    BarState& state = kitState<BarState>(*kit, node, MenuStateKey_Bar);
    if (state.open != kPrLayoutNodeNone && prKitIsOpen(kit, state.open)) {
        for (uint32_t i = 0; i < state.count; ++i) {
            PrLayoutNode entry = state.items[i];
            bool disabled = (prUiFlags(kit->ui, entry) & PrUiFlag_Disabled) != 0;
            if (entry == state.open || disabled || !prUiIsHovered(kit->ui, entry))
                continue;
            prKitSetOpen(kit, state.open, false);
            prKitSetOpen(kit, entry, true);
            break;
        }
    }
    state.open = kPrLayoutNodeNone;
    state.count = 0;
    return node;
}

PrLayoutNode prKitMenuBarItem(PrKit* kit, PrLayoutNode bar, PrKitMenuBarItem item) {
    Kit& k = *kit;
    Item entry = itemNode(k, bar, item.key, PrUiFlag_Pressable | PrUiFlag_Focusable, item.disabled);
    if (prUiActivated(k.ui, entry.node))
        prKitToggleOpen(kit, entry.node);
    bool open = prKitIsOpen(kit, entry.node);
    const PrKitPalette& p = k.theme.palette;
    const PrColor fills[] = {PrColor{0, 0, 0, 0}, p.bg3, p.bg4};
    ItemState lit = itemState(k, entry.node, entry.disabled);
    RowBox box{.height = kPrKitHeightLg, .padding = prKitEdgesXY(0, kPrKitSpace4)};
    prLayoutTreeFrameSetStyle(k.tree, entry.node,
        kitStyle(k, box, RowFlex{}, KitColors{.background = open ? p.bg3 : fills[lit]},
            KitFont{.role = PrKitTextRole_Body, .wrap = PrKitWrap_Nowrap}, KitInk{.color = open ? p.accent : p.text2}));
    prLayoutTreeFrameText(k.tree, entry.node, 0, item.text);

    BarState& state = kitState<BarState>(k, bar, MenuStateKey_Bar);
    if (state.count < kPrKitMenuBarMaxItems)
        state.items[state.count++] = entry.node;
    if (open)
        state.open = entry.node;
    return entry.node;
}

void prKitOpenContextMenu(PrKit* kit, PrLayoutNode host, PrVec2 at) {
    ContextState& state = contextOf(*kit, host);
    state.open = true;
    state.point = at;
    prUiRequestFrame(kit->ui);
}

void prKitCloseContextMenu(PrKit* kit, PrLayoutNode host) {
    contextOf(*kit, host).open = false;
}

PrLayoutNode prKitContextMenu(PrKit* kit, PrLayoutNode host, PrKitContextMenu menu, PrKitMenuStyle style) {
    PrUi* ui = kit->ui;
    prUiAddFlags(ui, host, PrUiFlag_Pressable);
    ContextState& state = contextOf(*kit, host);
    if (state.open && state.surface != kPrLayoutNodeNone) {
        bool escape = prUiKeyPressed(ui, state.surface, PrAppKey_Escape, 0);
        escape = prUiKeyPressed(ui, host, PrAppKey_Escape, 0) || escape;
        bool outside = prUiPressedOutside(ui, state.surface, 0) || prUiPressedOutside(ui, state.surface, 1);
        if (escape || outside)
            state.open = false;
    }
    PrVec2 pointer{};
    if (prUiClicked(ui, host, 1) && prUiPointer(ui, &pointer)) {
        state.open = true;
        state.point = pointer;
    }
    if (!state.open) {
        state.surface = kPrLayoutNodeNone;
        return kPrLayoutNodeNone;
    }
    PrRect at{state.point.x, state.point.y, state.point.x, state.point.y};
    PrLayoutNode surface = floating(*kit, floatKey(host, FloatKind_Context, menu.key), 0, at, PrVec2{},
        FloatLook_Surface, 0);
    if (state.surface != surface)
        prUiKeyPressed(ui, surface, PrAppKey_Escape, 0);
    state.surface = surface;
    kitProvide<Closer>(*kit, surface, KitContext_Closer) = Closer{.node = host, .key = MenuStateKey_Context};
    return prKitMenu(kit, surface, {}, style);
}

PrLayoutNode prKitTooltip(PrKit* kit, PrLayoutNode anchor, PrKitTooltip tooltip) {
    PrUi* ui = kit->ui;
    if (!prUiIsHovered(ui, anchor) || prUiIsPressed(ui, anchor, 0))
        return kPrLayoutNodeNone;
    if (prUiHoverSeconds(ui, anchor) < kPrKitTooltipDelay) {
        // The rest of the delay is timed frame by frame. A host that draws only on input would stop short of it.
        prUiRequestFrame(ui);
        return kPrLayoutNodeNone;
    }
    PrLayoutNode node = floating(*kit, floatKey(anchor, FloatKind_Tooltip, tooltip.key),
        PrUiFlag_Transparent | PrUiFlag_SubtreeTransparent, boundsOf(*kit, anchor),
        PrVec2{0, kPrKitSpace2}, FloatLook_Tooltip, 0);
    PrKitTextStyle text{.role = PrKitTextRole_BodySmall, .wrap = PrKitWrap_Nowrap, .preserve = true};
    prKitLabel(kit, node, {.text = tooltip.text}, text);
    if (tooltip.shortcut.size != 0)
        prKitKbd(kit, node, {.text = tooltip.shortcut});
    return node;
}

PrLayoutNode prKitDragGhost(PrKit* kit, PrKitDragGhost ghost) {
    const PrUiPayload* payload = prUiDragging(kit->ui);
    PrVec2 pointer{};
    if (payload == nullptr || !prUiPointer(kit->ui, &pointer))
        return kPrLayoutNodeNone;
    PrRect at{pointer.x, pointer.y, pointer.x, pointer.y};
    PrLayoutNode node = floating(*kit, floatKey(payload->source, FloatKind_Ghost, ghost.key),
        PrUiFlag_Transparent | PrUiFlag_SubtreeTransparent, at,
        PrVec2{kPrKitSpace6, kPrKitSpace6 - kPrKitHeightSm / 2}, FloatLook_Ghost, 0);
    if (ghost.icon != PrKitIconKind_None)
        prKitIcon(kit, node, {.icon = ghost.icon}, {.size = kItemIcon, .tone = PrKitTone_Secondary});
    PrKitTextStyle text{.role = PrKitTextRole_BodySmall, .wrap = PrKitWrap_Nowrap};
    prKitLabel(kit, node, {.text = ghost.text}, text);
    return node;
}

PrLayoutNode prKitModal(PrKit* kit, PrLayoutNode trigger, PrKitModal modal, PrKitModalStyle style) {
    PrUi* ui = kit->ui;
    OpenState& state = openOf(*kit, trigger);
    if (state.open) {
        // With nothing focused, a key goes to the root's claims.
        bool escape = prUiKeyPressed(ui, prLayoutTreeRoot(kit->tree), PrAppKey_Escape, 0);
        if (dismissed(*kit, state.surface, trigger) || escape)
            state.open = false;
    }
    if (!state.open) {
        state.surface = kPrLayoutNodeNone;
        return kPrLayoutNodeNone;
    }
    bool top = style.placement == PrKitModalPlacement_Top;
    ScrimBox scrim{.inset = top ? (style.inset > 0 ? style.inset : kModalInset) : 0, .top = top};
    PrLayoutStyleIndex record = kitStyle(*kit, scrim, ScrimFlex{.top = top}, KitColors{.background = kScrim});
    PrLayoutNode node =
        prLayoutTreeFrameElement(kit->tree, prUiOverlay(ui), floatKey(trigger, FloatKind_Modal, modal.key), record);
    PrLayoutNode content =
        prUiElement(ui, node, 0, kitStyle(*kit, FlexBox{}, ColumnFlex{.fixed = true}), PrUiFlag_FocusScope);
    if (state.surface != content)
        prUiKeyPressed(ui, content, PrAppKey_Escape, 0);
    state.surface = content;
    if (!prUiIsFocusWithin(ui, content)) {
        if (laidOut(*kit, content))
            prUiFocusMove(ui, content, false);
        else
            prUiRequestFrame(ui);
    }
    return content;
}

PrLayoutNode prKitDialog(PrKit* kit, PrLayoutNode trigger, PrKitDialog dialog, PrKitDialogStyle style) {
    PrLayoutNode content = prKitModal(kit, trigger, {.key = dialog.key}, style.modal);
    if (content == kPrLayoutNodeNone)
        return kPrLayoutNodeNone;
    PrLayoutNode frame =
        prLayoutTreeFrameElement(kit->tree, content, 0, cardStyle(*kit, style.width > 0 ? style.width : kDialogWidth));
    RowBox headerBox{.padding = prKitEdgesXY(kPrKitSpace4, kPrKitSpace5), .rules = PrKitRuleFlag_Bottom};
    PrLayoutNode header = prLayoutTreeFrameElement(kit->tree, frame, 0,
        ruledRow(*kit, headerBox, kPrKitSpace4, kit->theme.palette.bg2));
    PrKitTextStyle title{.role = PrKitTextRole_Heading, .wrap = PrKitWrap_Clip};
    prKitLabel(kit, header, {.text = dialog.title}, title);
    prKitSpacer(kit, header, {});
    PrLayoutNode closer = prKitButton(kit, header, {.icon = PrKitIconKind_Close},
        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
    if (prKitActivated(kit, closer))
        prKitSetOpen(kit, trigger, false);
    PrKitBoxStyle body{.gap = kPrKitSpace5, .padding = prKitEdgesAll(kPrKitSpace5)};
    return prKitBox(kit, frame, {}, body);
}

PrLayoutNode prKitDialogFooter(PrKit* kit, PrLayoutNode body) {
    PrKitBoxStyle footer{
        .direction = PrKitDirection_Row,
        .justify = PrKitJustify_End,
        .align = PrKitAlign_Center,
        .gap = kPrKitSpace3,
    };
    return prKitBox(kit, body, {}, footer);
}

PrLayoutNode prKitCommandPalette(PrKit* kit, PrLayoutNode trigger, PrKitCommandPalette palette) {
    PrLayoutNode content = prKitModal(kit, trigger, {.key = palette.key}, {.placement = PrKitModalPlacement_Top});
    if (content == kPrLayoutNodeNone)
        return kPrLayoutNodeNone;
    PrUi* ui = kit->ui;
    // The search field, or the frame if the palette has no query, takes focus as the modal's first focusable node. The
    // frame claims the keys that move and activate the selection, which reach it from the field inside it.
    bool queried = palette.query.data != nullptr;
    PrLayoutNode frame =
        prUiElement(ui, content, 0, cardStyle(*kit, kPaletteWidth), queried ? PrUiFlags(0) : PrUiFlag_Focusable);
    RowBox searchBox{.height = kPrKitHeightXl, .padding = prKitEdgesXY(0, kPrKitSpace5), .rules = PrKitRuleFlag_Bottom};
    PrLayoutNode search =
        prLayoutTreeFrameElement(kit->tree, frame, 0, ruledRow(*kit, searchBox, kPrKitSpace4, PrColor{0, 0, 0, 0}));
    prKitIcon(kit, search, {.icon = PrKitIconKind_Search}, {.size = 14, .tone = PrKitTone_Muted});
    PrLayoutNode queryField = kPrLayoutNodeNone;
    if (queried) {
        PrKitTextField field{.text = palette.query, .placeholder = palette.prompt};
        PrKitFieldStyle bare{.bare = true};
        queryField = prKitTextField(kit, search, field, bare);
    } else {
        PrKitTextStyle prompt{.tone = PrKitTone_Faint, .wrap = PrKitWrap_Clip};
        prKitLabel(kit, search, {.text = palette.prompt}, prompt);
        prKitSpacer(kit, search, {});
    }
    prKitKbd(kit, search, {.text = "ESC"});

    PrKitBoxStyle listBox{
        .overflow = PrKitOverflow_ScrollY,
        .padding = prKitEdgesXY(kPrKitSpace1, 0),
        .place = {.maxHeight = prKitPx(kPaletteListHeight), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode list = prKitBox(kit, frame, {}, listBox);
    PrStringView changed{};
    if (queryField != kPrLayoutNodeNone && prKitTextChanged(kit, queryField, &changed))
        kitReportText(*kit, list, changed);

    PaletteState& state = kitState<PaletteState>(*kit, frame, MenuStateKey_Palette);
    uint32_t selected = state.selected;
    if (prUiKeyPressed(ui, frame, PrAppKey_Down, 0) && selected + 1 < palette.count)
        ++selected;
    if (prUiKeyPressed(ui, frame, PrAppKey_Up, 0) && selected > 0)
        --selected;
    if (palette.count == 0 || selected >= palette.count)
        selected = palette.count != 0 ? palette.count - 1 : 0;
    bool entered = prUiKeyPressed(ui, frame, PrAppKey_Enter, 0);
    kitProvide<Palette>(*kit, list, KitContext_Palette) = Palette{
        .closer = Closer{.node = trigger, .key = MenuStateKey_Open},
        .frame = frame,
        .selected = selected,
        .entered = entered && palette.count != 0,
        .moved = selected != state.selected,
    };
    state.selected = selected;
    return list;
}

PrLayoutNode prKitCommandItem(PrKit* kit, PrLayoutNode list, PrKitCommandItem item) {
    Kit& k = *kit;
    PrUi* ui = k.ui;
    PrLayoutNode provider = kPrLayoutNodeNone;
    Palette* palette = kitContext<Palette>(k, list, KitContext_Palette, &provider);
    PR_ASSERT(palette != nullptr && provider == list);
    bool selected = palette->count++ == palette->selected;
    Item row = itemNode(k, list, item.key, PrUiFlag_Pressable, item.disabled);
    bool disabled = row.disabled;
    RowBox box{.padding = prKitEdgesXY(kPrKitSpace2, kPrKitSpace5)};
    KitColors fill{.background = itemFill(k, itemState(k, row.node, disabled), selected)};
    prLayoutTreeFrameSetStyle(k.tree, row.node, kitStyle(k, box, RowFlex{.gap = kPrKitSpace4}, fill));
    if (item.icon != PrKitIconKind_None)
        prKitIcon(kit, row.node, {.icon = item.icon},
            {.size = 14, .tone = PrKitTone(selected ? PrKitTone_Accent : PrKitTone_Muted)});
    PrKitTextStyle text{.tone = PrKitTone(disabled ? PrKitTone_Faint : PrKitTone_Primary), .wrap = PrKitWrap_Clip};
    prKitLabel(kit, row.node, {.text = item.text}, text);
    prKitSpacer(kit, row.node, {});
    if (item.category.size != 0) {
        PrKitTextStyle category{.role = PrKitTextRole_Meta};
        prKitLabel(kit, row.node, {.text = item.category}, category);
    }
    if (item.shortcut.size != 0)
        prKitKbd(kit, row.node, {.text = item.shortcut});

    if (selected && palette->moved)
        prUiReveal(ui, row.node);
    if (selected && palette->entered && !disabled)
        prUiActivate(ui, row.node);
    if (prUiActivated(ui, row.node))
        close(k, &palette->closer);
    return row.node;
}

PrLayoutNode prKitCommandPaletteFooter(PrKit* kit, PrLayoutNode list) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    const Palette* palette = kitContext<Palette>(*kit, list, KitContext_Palette, &provider);
    PR_ASSERT(palette != nullptr && provider == list);
    RowBox footerBox{.padding = prKitEdgesXY(kPrKitSpace3, kPrKitSpace5), .rules = PrKitRuleFlag_Top};
    PrLayoutNode footer = prLayoutTreeFrameElement(kit->tree, palette->frame, 0,
        ruledRow(*kit, footerBox, kPrKitSpace6, PrColor{0, 0, 0, 0}));
    struct Hint {
        PrStringView keys;
        PrStringView action;
    };
    const Hint hints[] = {{"Up Down", "Navigate"}, {"Ret", "Run"}, {"ESC", "Close"}};
    PrKitTextStyle caption{.role = PrKitTextRole_Caption};
    for (const Hint& hint : hints) {
        PrLayoutNode group = prKitBox(kit, footer, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace2});
        prKitKbd(kit, group, {.text = hint.keys});
        prKitLabel(kit, group, {.text = hint.action}, caption);
    }
    return footer;
}
