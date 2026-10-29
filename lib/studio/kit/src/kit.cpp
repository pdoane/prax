// Studio Kit: the kit's state, and what its widgets share to make their records and answer their input.

#include "pr/kit.h"
#include "studio/kit/src/kit.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// The durations in seconds of a hover's crossfade in and out, and of a press's crossfade.
constexpr double kHoverIn = 0.06;
constexpr double kHoverOut = 0.12;
constexpr double kPress = 0.05;

// Makes the text fonts of the theme's roles from its font set, and releases those of the previous theme.
void makeFonts(Kit& kit) {
    for (PrTextFont*& font : kit.fonts) {
        prTextFontRelease(font);
        font = nullptr;
    }
    if (kit.theme.fonts == nullptr)
        return;
    for (uint32_t role = 0; role < PrKitTextRole_Count; ++role) {
        const PrKitType& type = kit.theme.types[role];
        PrStringView name = type.mono ? kit.theme.mono : kit.theme.sans;
        PrFontFamily family = name.size != 0 ? prFontFamilyFromName(name)
                                             : prFontFamilyGeneric(type.mono ? PrFontGeneric_Monospace
                                                                             : PrFontGeneric_SansSerif);
        PrFontStyle style = prFontStyleDefault();
        style.size = type.size;
        style.weight = type.weight;
        if (type.italic)
            style.slope = PrFontSlope_Italic;
        PrTextFontDesc desc{.set = kit.theme.fonts, .families = &family, .familyCount = 1, .style = style};
        kit.fonts[role] = prTextFontCreate(&desc);
    }
}

// Makes the kit's looks from its theme's palette.
void makeLooks(Kit& kit) {
    const PrKitPalette& p = kit.theme.palette;
    PrColor clear{0, 0, 0, 0};
    auto look = [&](KitLookId id, PrColor rest, PrColor hover, PrColor press, PrColor border, PrColor text,
                    PrColor icon) {
        bool eases = !memEquals(&rest, &hover, sizeof rest) || !memEquals(&rest, &press, sizeof rest);
        kit.looks[id] =
            KitLook{.fill = {rest, hover, press}, .border = border, .text = text, .icon = icon, .eases = eases};
    };
    look(KitLookId_Button, p.bg3, p.bg4, p.bg2, p.line2, p.text1, p.text2);
    look(KitLookId_ButtonPrimary, p.accent, p.accentHover, p.accentPress, p.accent, p.accentText, p.textInverse);
    look(KitLookId_ButtonGhost, clear, p.bg3, p.bg4, clear, p.text2, p.text2);
    look(KitLookId_ButtonDanger, p.bg3, p.bg4, p.bg2, p.line2, p.danger, p.text2);
    look(KitLookId_ButtonSelected, p.accentWeak, p.accentWeak, p.accentWeak, clear, p.accent, p.accent);
    // A disabled control keeps its shape and loses its color: a quiet fill, or none for a ghost, and faint text.
    look(KitLookId_ButtonDisabled, p.bg2, p.bg2, p.bg2, p.line1, p.text4, p.text4);
    look(KitLookId_ButtonGhostDisabled, clear, clear, clear, clear, p.text4, p.text4);
    look(KitLookId_Selectable, clear, p.bg3, p.bg3, clear, p.text1, p.text2);
    look(KitLookId_SelectableSelected, p.accentWeak, p.accentWeak, p.accentWeak, clear, p.text1, p.accent);
    look(KitLookId_Plain, clear, p.bg4, p.bg2, clear, p.text3, p.text3);
    look(KitLookId_Quiet, clear, p.bg3, p.bg4, clear, p.text3, p.text3);
    look(KitLookId_Tab, clear, p.bg2, p.bg3, clear, p.text3, p.text3);
    look(KitLookId_TabChosen, clear, clear, clear, p.accent, p.text1, p.text1);
    look(KitLookId_TabDisabled, clear, clear, clear, clear, p.text4, p.text4);
    look(KitLookId_TabChosenDisabled, clear, clear, clear, p.accent, p.text4, p.text4);
    look(KitLookId_Segment, clear, p.bg3, p.bg4, clear, p.text2, p.text2);
    look(KitLookId_SegmentChosen, p.bg4, p.bg4, p.bg4, p.line3, p.text1, p.text1);
    look(KitLookId_SegmentDisabled, clear, clear, clear, clear, p.text4, p.text4);
    look(KitLookId_SegmentChosenDisabled, p.bg4, p.bg4, p.bg4, p.line3, p.text4, p.text4);
    look(KitLookId_Check, p.bg2, p.bg3, p.bg4, p.line3, p.text1, p.textInverse);
    look(KitLookId_CheckOn, p.accent, p.accentHover, p.accentPress, p.accent, p.text1, p.textInverse);
    look(KitLookId_CheckDisabled, p.bg2, p.bg2, p.bg2, p.line3, p.text4, p.textInverse);
    look(KitLookId_CheckOnDisabled, p.bg4, p.bg4, p.bg4, p.bg4, p.text4, p.textInverse);
    look(KitLookId_Rail, p.bg3, p.bg4, p.bg4, p.line3, p.text1, p.text2);
    look(KitLookId_RailDisabled, p.bg3, p.bg3, p.bg3, p.line3, p.text4, p.text2);
    look(KitLookId_Radio, p.bg2, p.bg3, p.bg4, p.line3, p.text1, p.text2);
    look(KitLookId_RadioOn, p.bg2, p.bg3, p.bg4, p.accent, p.text1, p.text2);
    look(KitLookId_RadioOnDisabled, p.bg2, p.bg2, p.bg2, p.bg4, p.text4, p.text2);
    // A row of a list marks the pointer at once, so that a pointer moving down a list leaves no trail.
    kit.looks[KitLookId_Selectable].eases = false;
}

// What a placed record is made from: the record it places, and the placement.
PR_NO_PADDING_BEGIN
struct Placement {
    PrLayoutStyleIndex base;
    PrKitPlace place;
};
PR_NO_PADDING_END

} // namespace

void KitBlock::make(const Kit&, PrLayoutBoxStyle& box) const {
    box.display = PrLayoutDisplay_Block;
}

void KitShrinkable::make(const Kit&, PrLayoutBoxStyle& box) const {
    box.display = PrLayoutDisplay_Block;
    box.minWidth = prLayoutLengthPx(0);
    box.minHeight = prLayoutLengthPx(0);
}

void KitFixed::make(const Kit&, PrLayoutFlexStyle& flex) const {
    flex.shrink = 0;
}

void KitGrow::make(const Kit&, PrLayoutFlexStyle& flex) const {
    flex.grow = 1;
    flex.basis = prLayoutLengthPx(0);
}

void KitFont::make(const Kit& kit, PrLayoutFontStyle& font) const {
    const PrKitType& type = kit.theme.types[role];
    font.font = kit.fonts[role];
    font.lineHeight = prLayoutLineHeightPx(type.lineHeight);
    font.whiteSpaceCollapse = preserve ? PrLayoutWhiteSpaceCollapse_Preserve : PrLayoutWhiteSpaceCollapse_Collapse;
    PrKitWrap resolved = kitTextWrap(kit, role, wrap);
    font.textWrapMode = resolved == PrKitWrap_Wrap ? PrLayoutTextWrapMode_Wrap : PrLayoutTextWrapMode_Nowrap;
    static const PrLayoutTextAlign kAlign[] = {PrLayoutTextAlign_Start, PrLayoutTextAlign_Center,
                                               PrLayoutTextAlign_End};
    font.textAlign = font.textAlignLast = kAlign[align];
}

void KitInk::make(const Kit&, PrLayoutColorStyle& value) const {
    value.color = kitColor16(color);
}

void KitColors::make(const Kit&, PrLayoutDecorationStyle& decoration) const {
    decoration.background = kitColor16(background);
    kitSetBorderColor(decoration, border);
}

void KitToneInk::make(const Kit& kit, PrLayoutColorStyle& color) const {
    color.color = kitColor16(kitToneColor(kit, tone));
}

void KitContainerBox::make(const Kit&, PrLayoutBoxStyle& box) const {
    box.display = PrLayoutDisplay_Flex;
    kitSetPadding(box, style.padding);
    switch (style.overflow) {
        case PrKitOverflow_Clip:
            box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
            break;
        case PrKitOverflow_Scroll:
            box.overflowX = box.overflowY = PrLayoutOverflow_Scroll;
            break;
        case PrKitOverflow_ScrollY:
            box.overflowX = PrLayoutOverflow_Hidden;
            box.overflowY = PrLayoutOverflow_Scroll;
            break;
        default:
            break;
    }
    // A box that clips or scrolls lets its parent shrink it past its content.
    if (style.overflow != PrKitOverflow_Visible) {
        box.minWidth = prLayoutLengthPx(0);
        box.minHeight = prLayoutLengthPx(0);
    }
    kitPlaceBox(box, style.place);
}

void KitContainerFlex::make(const Kit&, PrLayoutFlexStyle& value) const {
    value.direction = direction == PrKitDirection_Row ? PrLayoutFlexDirection_Row : PrLayoutFlexDirection_Column;
    value.wrap = wrap ? PrLayoutFlexWrap_Wrap : PrLayoutFlexWrap_NoWrap;
    static const PrLayoutAlignment kJustify[] = {
        PrLayoutAlignment_FlexStart,    PrLayoutAlignment_FlexEnd,     PrLayoutAlignment_Center,
        PrLayoutAlignment_SpaceBetween, PrLayoutAlignment_SpaceAround, PrLayoutAlignment_SpaceEvenly,
    };
    value.justifyContent = kJustify[justify];
    static const PrLayoutAlignment kAlign[] = {
        PrLayoutAlignment_Stretch, PrLayoutAlignment_Stretch, PrLayoutAlignment_FlexStart,
        PrLayoutAlignment_FlexEnd, PrLayoutAlignment_Center,  PrLayoutAlignment_Baseline,
    };
    value.alignItems = kAlign[align];
    value.rowGap = prLayoutLengthPx(gap);
    value.columnGap = prLayoutLengthPx(gap);
    kitPlaceFlex(value, PrKitPlace{.flex = flex, .align = alignSelf, .weight = weight});
}

void KitPlacedBox::make(const Kit& kit, PrLayoutBoxStyle& box) const {
    box = prLayoutBoxStyleRecord(kit.tree, base);
    kitPlaceBox(box, place);
}

void KitPlacedFlex::make(const Kit& kit, PrLayoutFlexStyle& flex) const {
    flex = prLayoutFlexStyleRecord(kit.tree, base);
    kitPlaceFlex(flex, place);
}

void kitSetPadding(PrLayoutBoxStyle& box, PrKitEdges padding) {
    box.padding[PrRectSide_Top] = prLayoutLengthPx(padding.top);
    box.padding[PrRectSide_Right] = prLayoutLengthPx(padding.right);
    box.padding[PrRectSide_Bottom] = prLayoutLengthPx(padding.bottom);
    box.padding[PrRectSide_Left] = prLayoutLengthPx(padding.left);
}

void kitSetBorderWidth(PrLayoutBoxStyle& box, float width) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.border[side] = prLayoutUnitFromPx(width);
}

void kitSetBorderColor(PrLayoutDecorationStyle& decoration, PrColor color) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        decoration.borderColor[side] = kitColor16(color);
        decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
    }
}

void kitPlaceBox(PrLayoutBoxStyle& box, const PrKitPlace& place) {
    if (place.width.kind != PrKitSizeKind_Auto)
        box.width = kitLength(place.width);
    if (place.height.kind != PrKitSizeKind_Auto)
        box.height = kitLength(place.height);
    if (place.minWidth.kind != PrKitSizeKind_Auto)
        box.minWidth = kitLength(place.minWidth);
    if (place.minHeight.kind != PrKitSizeKind_Auto)
        box.minHeight = kitLength(place.minHeight);
    if (place.maxWidth.kind != PrKitSizeKind_Auto)
        box.maxWidth = kitLength(place.maxWidth);
    if (place.maxHeight.kind != PrKitSizeKind_Auto)
        box.maxHeight = kitLength(place.maxHeight);
    const float margins[PrRectSide_Count] = {place.margin.top, place.margin.right, place.margin.bottom,
                                             place.margin.left};
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        if (margins[side] != 0)
            box.margin[side] = prLayoutLengthPx(margins[side]);
    }
    // An item that fills takes a share of the room regardless of its content's minimum.
    if (place.flex == PrKitFlex_Fill) {
        box.minWidth = prLayoutLengthPx(0);
        box.minHeight = prLayoutLengthPx(0);
    }
}

void kitPlaceFlex(PrLayoutFlexStyle& flex, const PrKitPlace& place) {
    float share = place.weight > 0 ? place.weight : 1;
    switch (place.flex) {
        case PrKitFlex_Auto:
            flex.grow = 0;
            flex.shrink = 1;
            flex.basis = prLayoutLengthAuto();
            break;
        case PrKitFlex_Fixed:
            flex.grow = 0;
            flex.shrink = 0;
            flex.basis = prLayoutLengthAuto();
            break;
        case PrKitFlex_Fill:
            flex.grow = share;
            flex.shrink = 1;
            flex.basis = prLayoutLengthPx(0);
            break;
        case PrKitFlex_Grow:
            flex.grow = share;
            flex.shrink = 1;
            flex.basis = prLayoutLengthAuto();
            break;
        default:
            break;
    }
    static const PrLayoutAlignment kAlign[] = {
        PrLayoutAlignment_Auto,    PrLayoutAlignment_Stretch, PrLayoutAlignment_FlexStart,
        PrLayoutAlignment_FlexEnd, PrLayoutAlignment_Center,  PrLayoutAlignment_Baseline,
    };
    if (place.align != PrKitAlign_Auto)
        flex.alignSelf = kAlign[place.align];
}

PrLayoutNode kitBoxNode(Kit& kit, PrLayoutNode parent, PrKitBox box, PrKitOverflow overflow, PrLayoutStyleIndex style) {
    PrLayoutNode node = prLayoutTreeFrameElement(kit.tree, parent, box.key, style);
    PrUiFlags flags = overflow == PrKitOverflow_Scroll || overflow == PrKitOverflow_ScrollY ? PrUiFlag_Scrollable : 0;
    if (box.disabled)
        flags |= PrUiFlag_Disabled;
    if (flags != 0)
        prUiSetFlags(kit.ui, node, flags);
    return node;
}

KitControl kitControl(Kit& kit, PrLayoutNode parent, uint64_t key, PrUiFlags flags, bool disabled) {
    return kitControlFromNode(kit, prLayoutTreeFrameElement(kit.tree, parent, key, 0), flags, disabled);
}

KitControl kitControlFromNode(Kit& kit, PrLayoutNode node, PrUiFlags flags, bool disabled) {
    if (disabled)
        flags |= PrUiFlag_Disabled;
    prUiSetFlags(kit.ui, node, flags);
    KitControl c{.node = node};
    c.disabled = prUiIsDisabled(kit.ui, node);
    if (c.disabled)
        return c;
    if ((flags & PrUiFlag_Pressable) != 0)
        prUiSetCursor(kit.ui, node, PrAppCursor_Hand);
    c.hover = kitHoverFactor(kit, node);
    c.press = kitPressFactor(kit, node);
    c.over = prUiIsHovered(kit.ui, node);
    c.down = prUiIsPressed(kit.ui, node, 0);
    c.ring = prUiIsFocusVisible(kit.ui) && prUiFocus(kit.ui) == node;
    return c;
}

void KitFill::make(const Kit& kit, PrLayoutDecorationStyle& decoration) const {
    const KitLook& l = kit.looks[look];
    PrColor fill = l.fill[state];
    if (hover > 0 || press > 0)
        fill = kitMix(kitMix(l.fill[KitState_Rest], l.fill[KitState_Hover], hover), l.fill[KitState_Press], press);
    decoration.background = kitColor16(fill);
    kitSetBorderColor(decoration, l.border);
    if (ring) {
        decoration.outlineColor = kitColor16(kit.theme.palette.accent);
        decoration.outlineStyle = PrDrawBorderStyle_Solid;
        decoration.outlineWidth = prLayoutUnitFromPx(1);
        decoration.outlineOffset = prLayoutUnitFromPx(1);
    }
}

KitDress kitDress(const Kit& kit, const KitControl& control, KitLookId look) {
    KitState state = KitState_Rest;
    bool settled = kitSettled(kit, control, look, &state);
    KitFill fill{.look = look,
                 .state = state,
                 .ring = control.ring,
                 .hover = settled ? 0 : control.hover,
                 .press = settled ? 0 : control.press};
    return KitDress{.fill = fill, .ink = KitInk{.color = kit.looks[look].text}};
}

bool kitSettled(const Kit& kit, const KitControl& control, KitLookId look, KitState* state) {
    if (!kit.looks[look].eases) {
        *state = control.down ? KitState_Press : control.over ? KitState_Hover : KitState_Rest;
        return true;
    }
    bool settled = (control.hover == 0 || control.hover == 1) && (control.press == 0 || control.press == 1);
    *state = control.press == 1 ? KitState_Press : control.hover == 1 ? KitState_Hover : KitState_Rest;
    return settled;
}

void kitSignal(Kit& kit, PrLayoutNode node, KitSignals signals) {
    prUiSignal(kit.ui, node, &kKitOwner, signals);
}

bool kitSignaled(const Kit& kit, PrLayoutNode node, KitSignals signals) {
    return prUiSignaled(kit.ui, node, &kKitOwner, signals);
}

bool kitSetFlags(Kit& kit, PrLayoutNode node, PrUiFlags flags) {
    prUiSetFlags(kit.ui, node, flags);
    return prUiIsDisabled(kit.ui, node);
}

PrLayoutLength kitLength(PrKitSize size) {
    switch (size.kind) {
        case PrKitSizeKind_Px:
            return prLayoutLengthPx(size.value);
        case PrKitSizeKind_Percent:
            return prLayoutLengthPercent(size.value);
        case PrKitSizeKind_Fill:
            return prLayoutLengthStretch();
        default:
            return prLayoutLengthAuto();
    }
}

PrColor16 kitColor16(PrColor color) {
    return prColor16FromColor(color);
}

PrColor kitMix(PrColor from, PrColor to, float t) {
    // The channels mix weighted by their alphas, as premultiplied colors do, so that a transparent end adds coverage
    // and no color: a fade from clear to gray is gray throughout, not darker on the way.
    float a = from.a + (to.a - from.a) * t;
    if (a <= 0)
        return PrColor{to.r, to.g, to.b, 0};
    auto mix = [&](float x, float y) { return (x * from.a + (y * to.a - x * from.a) * t) / a; };
    return PrColor{mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b), a};
}

PrColor kitToneColor(const Kit& kit, PrKitTone tone) {
    const PrKitPalette& p = kit.theme.palette;
    switch (tone) {
        case PrKitTone_Secondary:
            return p.text2;
        case PrKitTone_Muted:
            return p.text3;
        case PrKitTone_Faint:
            return p.text4;
        case PrKitTone_Accent:
            return p.accent;
        case PrKitTone_Success:
            return p.success;
        case PrKitTone_Warning:
            return p.warning;
        case PrKitTone_Danger:
            return p.danger;
        case PrKitTone_Inverse:
            return p.textInverse;
        case PrKitTone_AxisX:
            return p.axisX;
        case PrKitTone_AxisY:
            return p.axisY;
        case PrKitTone_AxisZ:
            return p.axisZ;
        case PrKitTone_AxisW:
            return p.axisW;
        default:
            return p.text1;
    }
}

PrKitTone kitTextTone(const Kit& kit, PrKitTextRole role, PrKitTone tone) {
    if (tone != PrKitTone_Auto)
        return tone;
    PrKitTone own = kit.theme.types[role].tone;
    return own != PrKitTone_Auto ? own : PrKitTone(PrKitTone_Primary);
}

PrKitWrap kitTextWrap(const Kit& kit, PrKitTextRole role, PrKitWrap wrap) {
    if (wrap != PrKitWrap_Auto)
        return wrap;
    PrKitWrap own = kit.theme.types[role].wrap;
    return own != PrKitWrap_Auto ? own : PrKitWrap(PrKitWrap_Wrap);
}

float kitHoverFactor(Kit& kit, PrLayoutNode node) {
    bool over = prUiIsHovered(kit.ui, node) && (prUiFlags(kit.ui, node) & PrUiFlag_Disabled) == 0;
    return prUiTween(kit.ui, node, &kKitOwner, KitStateKey_Hover, over ? 1.0f : 0.0f, over ? kHoverIn : kHoverOut,
                     nullptr);
}

float kitPressFactor(Kit& kit, PrLayoutNode node) {
    bool down = prUiIsPressed(kit.ui, node, 0);
    return prUiTween(kit.ui, node, &kKitOwner, KitStateKey_Press, down ? 1.0f : 0.0f, kPress, nullptr);
}

PrVec2 kitExtent(const Kit& kit, PrLayoutNode node) {
    PrLayoutRect box = prLayoutNodeBounds(kit.tree, node, PrLayoutCoordinates_Layout);
    return prLayoutVecToPx(PrLayoutVec{box.right - box.left, box.bottom - box.top});
}

float kitControlHeight(PrKitControlSize size) {
    switch (size) {
        case PrKitControlSize_Sm:
            return kPrKitHeightSm;
        case PrKitControlSize_Xs:
            return kPrKitHeightXs;
        case PrKitControlSize_Lg:
            return kPrKitHeightLg;
        default:
            return kPrKitHeightMd;
    }
}

} // namespace pr_impl

using namespace pr_impl;

PrKit* prKitCreate(PrUi* ui, const PrKitTheme* theme) {
    PrKit* kit = allocZeroed<PrKit>();
    kit->ui = ui;
    kit->tree = prUiTree(ui);
    prKitSetTheme(kit, theme);
    return kit;
}

void prKitDestroy(PrKit* PR_NULLABLE kit) {
    if (kit == nullptr)
        return;
    prUiForget(kit->ui, static_cast<Kit*>(kit));
    for (PrTextFont* font : kit->fonts)
        prTextFontRelease(font);
    for (PrPath* path : kit->icons)
        prPathDestroy(path);
    for (uint32_t i = 0; i < kit->painted.count; ++i)
        prMemoryFree(kit->painted.data[i]);
    kit->painted.reset();
    kit->paintedIndex.reset();
    prMemoryFree(kit);
}

void prKitSetTheme(PrKit* kit, const PrKitTheme* theme) {
    prUiForget(kit->ui, static_cast<Kit*>(kit));
    kit->theme = *theme;
    makeFonts(*kit);
    makeLooks(*kit);
}

const PrKitTheme* prKitTheme(const PrKit* kit) {
    return &kit->theme;
}

PrUi* prKitUi(const PrKit* kit) {
    return kit->ui;
}

PrLayoutTree* prKitTree(const PrKit* kit) {
    return kit->tree;
}

bool prKitActivated(const PrKit* kit, PrLayoutNode node) {
    return prUiActivated(kit->ui, node);
}

bool prKitRemoved(const PrKit* kit, PrLayoutNode node) {
    return kitSignaled(*kit, node, KitSignal_Removed);
}

void prKitPlace(PrKit* kit, PrLayoutNode node, const PrKitPlace* place) {
    // The node's record with its box and flex groups placed, and the rest the record's own. The memo keys the placed
    // record by the record it was placed from, to which its entry holds a reference.
    Kit& k = *kit;
    PrLayoutTree* tree = k.tree;
    Placement key{.base = prLayoutTreeFrameStyle(tree, node), .place = *place};
    const void* type = uiTypeId<Placement>();
    PrLayoutStyleIndex placed = 0;
    const void* kept = prUiMemoFind(k.ui, type, &k, &placed);
    if (kept == nullptr || !memEquals(kept, &key, sizeof key)) {
        PrUiPart part{.type = type, .owner = &k, .bytes = &key, .size = uint32_t(sizeof key)};
        PrLayoutStyle record = prLayoutStyleRecord(tree, key.base);
        PrLayoutGroupIndex& box = record.groups[PrLayoutGroup_Box];
        PrLayoutGroupIndex& flex = record.groups[PrLayoutGroup_Flex];
        box = kitGroup(k, KitPlacedBox{.base = box, .place = *place});
        flex = kitGroup(k, KitPlacedFlex{.base = flex, .place = *place});
        placed = prUiMemoKeep(k.ui, &part, prLayoutStyleIntern(tree, &record), key.base);
    }
    prLayoutTreeFrameSetStyle(tree, node, placed);
}
