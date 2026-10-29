// Studio Kit's buttons: text and icon buttons, button groups, and the badges, key caps, statuses and chips beside them.

#include "pr/kit_button.h"

#include "studio/kit/src/kit.h"
#include "studio/kit/src/kit_icon.h"

namespace pr_impl {

namespace {

// The look of each PrKitButtonVariant.
constexpr KitLookId kVariantLooks[] = {KitLookId_Button, KitLookId_ButtonPrimary, KitLookId_ButtonGhost,
                                       KitLookId_ButtonDanger};

// The size of a button's icon by PrKitControlSize, which grows with the button.
constexpr float kIconSizes[] = {14, 12, 10, 16};

// Returns the padding at the ends of a button of `size`, which grows with the button.
float endPadding(PrKitControlSize size) {
    switch (size) {
        case PrKitControlSize_Sm:
            return kPrKitSpace4;
        case PrKitControlSize_Xs:
            return kPrKitSpace3;
        case PrKitControlSize_Lg:
            return kPrKitSpace6;
        default:
            return kPrKitSpace5;
    }
}

// Returns the look of a button of `variant`: toggled on if `selected`, and quiet if `disabled`.
KitLookId buttonLook(PrKitButtonVariant variant, bool selected, bool disabled) {
    if (disabled)
        return variant == PrKitButtonVariant_Ghost ? KitLookId_ButtonGhostDisabled : KitLookId_ButtonDisabled;
    return selected ? KitLookId_ButtonSelected : kVariantLooks[variant];
}

// A button's box: as tall as its size, as wide as tall if it shows an icon alone, and bordered unless it shares a
// group's border.
PR_NO_PADDING_BEGIN
struct ButtonBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitControlSize size;
    bool square;
    bool grouped;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        float height = kitControlHeight(size);
        float ends = square ? 0 : endPadding(size);
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(height);
        if (square)
            box.width = prLayoutLengthPx(height);
        kitSetPadding(box, PrKitEdges{0, ends, 0, ends});
        if (!grouped)
            kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END

// A button's flex: its icon and label centered, apart by a gap, and the button never shrinks.
struct ButtonFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kPrKitSpace3);
        flex.shrink = 0;
    }
};

// Returns the font of a button's label, which its text node takes: smaller for the small sizes, and never wrapping.
KitFont buttonFont(PrKitControlSize size) {
    bool small = size == PrKitControlSize_Xs || size == PrKitControlSize_Sm;
    return KitFont{.role = PrKitTextRole(small ? PrKitTextRole_BodySmall : PrKitTextRole_Body),
                   .wrap = PrKitWrap_Nowrap};
}

// What a button group gives the buttons made directly under it.
struct GroupContext {
    uint8_t unused;
};

bool inGroup(const Kit& kit, PrLayoutNode parent) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    return kitContext<GroupContext>(kit, parent, KitContext_ButtonGroup, &provider) != nullptr && provider == parent;
}

// A button group's groups: a bordered row whose fill shows through the gap between its buttons as the rule that divides
// them.
struct GroupBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        kitSetBorderWidth(box, 1);
    }
};
struct GroupFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Stretch;
        flex.columnGap = prLayoutLengthPx(1);
        flex.shrink = 0;
    }
};

// The small tags: badges, key caps, statuses and chips. Each is a row of short text that does not shrink.
enum : uint8_t {
    Tag_Badge,
    Tag_Kbd,
    Tag_Status,
    Tag_Chip,
};
typedef uint8_t Tag;

PR_NO_PADDING_BEGIN
struct TagBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    Tag tag;

    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        switch (tag) {
            case Tag_Badge:
                box.height = prLayoutLengthPx(16);
                kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace3));
                break;
            case Tag_Kbd:
                box.height = prLayoutLengthPx(16);
                kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace2));
                kitSetBorderWidth(box, 1);
                break;
            case Tag_Status:
                box.height = prLayoutLengthPx(kPrKitHeightXxs);
                box.minWidth = prLayoutLengthPx(kPrKitHeightXxs);
                kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace1));
                break;
            default:
                box.height = prLayoutLengthPx(kPrKitHeightXs + 2);
                kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace3));
                kitSetBorderWidth(box, 1);
                break;
        }
    }
};
struct TagFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    Tag tag;

    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.shrink = 0;
        if (tag == Tag_Status)
            flex.justifyContent = PrLayoutAlignment_Center;
        if (tag == Tag_Chip)
            flex.columnGap = prLayoutLengthPx(kPrKitSpace2);
    }
};
PR_NO_PADDING_END

// Returns the font of a tag's text.
KitFont tagFont(Tag tag) {
    PrKitTextRole role = PrKitTextRole_Meta;
    if (tag == Tag_Status)
        role = PrKitTextRole_MonoStrong;
    else if (tag == Tag_Chip)
        role = PrKitTextRole_BodySmall;
    return KitFont{.role = role, .wrap = PrKitWrap_Nowrap};
}

// Returns `color` at `alpha`: the wash of a tone behind its text.
PrColor washOf(PrColor color, float alpha) {
    color.a = alpha;
    return color;
}

// Makes a tag of `text` in the record of its groups.
PrLayoutNode tag(Kit& kit, PrLayoutNode parent, uint64_t key, PrStringView text, Tag tag, PrColor ink,
                 KitColors colors) {
    PrLayoutStyleIndex record =
        kitStyle(kit, TagBox{.tag = tag}, TagFlex{.tag = tag}, tagFont(tag), KitInk{.color = ink}, colors);
    PrLayoutNode node = prLayoutTreeFrameElement(kit.tree, parent, key, record);
    if (text.size != 0)
        prLayoutTreeFrameText(kit.tree, node, 0, text);
    return node;
}

// The tone of each PrKitBadgeVariant.
constexpr PrKitTone kBadgeTones[] = {PrKitTone_Secondary, PrKitTone_Accent, PrKitTone_Success, PrKitTone_Warning,
                                     PrKitTone_Danger};

// A chip's remove control: a square as tall as the smallest control, which centers its icon.
struct RemoveBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const Kit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.width = box.height = prLayoutLengthPx(kPrKitHeightXxs);
    }
};
struct RemoveFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const Kit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_Center;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitButton(PrKit* kit, PrLayoutNode parent, PrKitButton button, PrKitButtonStyle style) {
    Kit& k = *kit;
    KitControl c = kitControl(k, parent, button.key, PrUiFlag_Pressable | PrUiFlag_Focusable, button.disabled);
    ButtonBox box{.size = style.size,
                  .square = button.text.size == 0 && button.icon != PrKitIconKind_None,
                  .grouped = inGroup(k, parent)};
    KitLookId look = buttonLook(style.variant, button.selected, c.disabled);
    KitDress dress = kitDress(k, c, look);
    prLayoutTreeFrameSetStyle(k.tree, c.node,
                              kitStyle(k, box, ButtonFlex{}, buttonFont(style.size), dress.fill, dress.ink));
    if (button.icon != PrKitIconKind_None)
        kitIcon(k, c.node, 0, button.icon, kIconSizes[style.size], k.looks[look].icon);
    if (button.text.size != 0)
        prLayoutTreeFrameText(k.tree, c.node, 0, button.text);
    return c.node;
}

PrLayoutNode prKitBadge(PrKit* kit, PrLayoutNode parent, PrKitBadge badge, PrKitBadgeStyle style) {
    PrColor ink = kitToneColor(*kit, kBadgeTones[style.variant]);
    PrColor fill = style.variant == PrKitBadgeVariant_Neutral ? kit->theme.palette.bg3 : washOf(ink, 0.16f);
    return tag(*kit, parent, badge.key, badge.text, Tag_Badge, ink, KitColors{.background = fill});
}

PrLayoutNode prKitKbd(PrKit* kit, PrLayoutNode parent, PrKitKbd kbd) {
    const PrKitPalette& p = kit->theme.palette;
    return tag(*kit, parent, kbd.key, kbd.text, Tag_Kbd, p.text3, KitColors{.border = p.line2});
}

PrLayoutNode prKitStatus(PrKit* kit, PrLayoutNode parent, PrKitStatus status, PrKitStatusStyle style) {
    PrColor ink = kitToneColor(*kit, style.tone);
    return tag(*kit, parent, status.key, status.text, Tag_Status, ink, KitColors{.background = washOf(ink, 0.18f)});
}

PrLayoutNode prKitChip(PrKit* kit, PrLayoutNode parent, PrKitChip chip) {
    Kit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    PrLayoutNode node =
        tag(k, parent, chip.key, chip.label, Tag_Chip, p.text1, KitColors{.background = p.bg3, .border = p.line2});
    if (!chip.removable)
        return node;
    KitControl remove = kitControl(k, node, 0, PrUiFlag_Pressable, chip.disabled);
    KitLookId look = remove.disabled ? KitLookId_ButtonGhostDisabled : KitLookId_Plain;
    prLayoutTreeFrameSetStyle(k.tree, remove.node,
                              kitStyle(k, RemoveBox{}, RemoveFlex{}, kitDress(k, remove, look).fill));
    kitIcon(k, remove.node, 0, PrKitIconKind_Close, 10, k.looks[look].icon);
    if (prUiActivated(k.ui, remove.node))
        kitSignal(k, node, KitSignal_Removed);
    return node;
}

PrLayoutNode prKitButtonGroup(PrKit* kit, PrLayoutNode parent, PrKitButtonGroup group) {
    Kit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    PrLayoutStyleIndex record =
        kitStyle(k, GroupBox{}, GroupFlex{}, KitColors{.background = p.line2, .border = p.line2});
    PrLayoutNode node =
        kitBoxNode(k, parent, PrKitBox{.key = group.key, .disabled = group.disabled}, PrKitOverflow_Visible, record);
    kitProvide<GroupContext>(k, node, KitContext_ButtonGroup);
    return node;
}
