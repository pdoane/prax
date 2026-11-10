// Studio Kit's tabs, segmented controls, checkboxes, toggles and radios, progress and meters.

#include "pr/kit_choice.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/geom.h"
#include "pr/hash.h"
#include "pr/layout_query.h"
#include "pr/layout_unit.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

// A tab that a strip holds: its id, and its node in `round`, the last round that created it.
struct HeldTab {
    uint64_t id;
    PrLayoutNode node;
    uint32_t round;
};

// The tabs of a strip (PrKitTabs), in the strip's order, and the selected tab. `round` counts the rounds that declared
// the strip. `focusing` is a tab that an arrow key selected before the round created it, which takes focus as the round
// creates it, or 0.
struct KitTabs {
    RawArray<HeldTab> order;
    uint64_t selected;
    uint64_t focusing;
    uint32_t round;
};

namespace {

// The retained state of the choices on a node.
enum : uint64_t {
    ChoiceStateKey_Knob = KitStateKey_Choice,
    ChoiceStateKey_TabStrip,
};
typedef uint64_t ChoiceStateKey;

// The heights in px of a checkbox, a radio's ring and a toggle's track, the track's width, the knob's extent, and the
// knob's travel along the track.
constexpr float kGlyph = 14;
constexpr float kRailWidth = 26;
constexpr float kKnob = 10;
constexpr float kKnobTravel = kRailWidth - kKnob - 4;

// The number of seconds that a knob takes to cross its track.
constexpr double kKnobSeconds = 0.12;

// The height in px of a progress bar and of a meter.
constexpr float kBarHeight = 6;

// The number of seconds that an indeterminate band takes to cross its track, and the share of the track that the band
// covers.
constexpr double kSweepSeconds = 1.2;
constexpr float kSweepShare = 0.3f;

// The number of seconds that a spinner takes to turn once, and the number of steps per turn at which it is drawn, so
// that each step's effects are kept.
constexpr double kSpinSeconds = 1.0;
constexpr uint32_t kSpinSteps = 48;

// The side in px of a spinner's square if its style gives none.
constexpr float kSpinnerSize = 16;

// The distance in px that a pressed tab moves before it is dragged.
constexpr float kTabDragThreshold = 4;

constexpr float kTurn = 6.28318530718f;

float unitClamp(float value) {
    return value > 0 ? (value < 1 ? value : 1) : 0;
}

// Returns how far `seconds` is through a period of `period`, as a fraction.
float phaseOf(double seconds, double period) {
    double turns = seconds / period;
    return float(turns - double(int64_t(turns)));
}

// Returns the font of a choice's label: `role`, on one line.
KitFont choiceFont(PrKitTextRole role) {
    return KitFont{.role = role, .wrap = PrKitWrap_Nowrap};
}

// A tab strip: a row that its caret is placed in, which gives way to its parent, with a rule below it. A tab strip, as
// tall as a tab while it is empty, so that a tab can be dropped on it.
PR_NO_PADDING_BEGIN
struct StripBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitControlSize size;
    bool collapsed;                 // the strip takes no room

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = collapsed ? PrLayoutDisplay_None : PrLayoutDisplay_Flex;
        box.position = PrLayoutPosition_Relative;
        box.minWidth = prLayoutLengthPx(0);
        box.minHeight = prLayoutLengthPx(kitControlHeight(size) + 4);
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(1);
    }
};
PR_NO_PADDING_END

// A tab: a row a little taller than its control size, padded at its ends, with a rule below it two px thick that its
// look draws while it is chosen, so that choosing a tab moves no content.
PR_NO_PADDING_BEGIN
struct TabBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitControlSize size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(kitControlHeight(size) + 4);
        kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace6 + 1));
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(2);
    }
};
PR_NO_PADDING_END
struct TabFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    int32_t order;                  // the tab's place in its strip's order

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kPrKitSpace3);
        flex.shrink = 0;
        flex.order = order;
    }
};

// Where a dragged tab goes: a bar two px wide at `left` px in the strip, which follows the pointer.
PR_NO_PADDING_BEGIN
struct CaretBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float left;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.width = prLayoutLengthPx(2);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(kPrKitSpace1);
        box.inset[PrRectSide_Bottom] = prLayoutLengthPx(kPrKitSpace2);
        box.inset[PrRectSide_Left] = prLayoutLengthPx(left - 1);
    }
};
PR_NO_PADDING_END

// A segmented control's track: a bordered row as tall as its control size, which its segments fill.
PR_NO_PADDING_BEGIN
struct TrackBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitControlSize size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(kitControlHeight(size));
        box.minWidth = prLayoutLengthPx(0);
        kitSetPadding(box, prKitEdgesAll(1));
        kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END
struct TrackFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Stretch;
        flex.shrink = 0;
    }
};

// A segment: a bordered share of its track, which centers its icon and label.
struct SegmentBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.minWidth = prLayoutLengthPx(0);
        kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace5));
        kitSetBorderWidth(box, 1);
    }
};
struct SegmentFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.grow = 1;
        flex.basis = prLayoutLengthPx(0);
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kPrKitSpace2);
    }
};

// A radio group: its radios along `direction`.
PR_NO_PADDING_BEGIN
struct GroupFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    PrKitDirection direction;

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.direction = direction == PrKitDirection_Row ? PrLayoutFlexDirection_Row : PrLayoutFlexDirection_Column;
        flex.alignItems = PrLayoutAlignment_FlexStart;
        flex.rowGap = prLayoutLengthPx(0);
        flex.columnGap = prLayoutLengthPx(kPrKitSpace6);
    }
};
PR_NO_PADDING_END

// A flex container of the display's default, for a part that sets nothing else in its box.
struct FlexBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
    }
};

// A checkbox's, toggle's or radio's row: its glyph and its label, as tall as its control size.
PR_NO_PADDING_BEGIN
struct RowBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitControlSize size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.height = prLayoutLengthPx(kitControlHeight(size));
    }
};
PR_NO_PADDING_END
struct RowFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kPrKitSpace4);
        flex.shrink = 0;
    }
};

// A glyph: a box of fixed extent, bordered if `bordered` and padded by `padding` px, which centers its content.
PR_NO_PADDING_BEGIN
struct GlyphBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;
    float height;
    float padding;
    bool bordered;
    uint8_t _pad[3];

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.width = prLayoutLengthPx(width);
        box.height = prLayoutLengthPx(height);
        kitSetPadding(box, prKitEdgesAll(padding));
        if (bordered)
            kitSetBorderWidth(box, 1);
    }
};
struct GlyphFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    bool atStart;                   // the content sits at the start, as a toggle's knob does

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.shrink = 0;
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = atStart ? PrLayoutAlignment_FlexStart : PrLayoutAlignment_Center;
    }
};
PR_NO_PADDING_END

// A toggle's knob, moved `travel` of the way along its track.
PR_NO_PADDING_BEGIN
struct KnobEffects {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    float travel;

    void make(const PrKit&, PrLayoutEffectStyle& effects) const {
        if (travel != 0)
            effects.transform = prLayoutTransformFromAffine2(prAffine2Translate(PrVec2{travel * kKnobTravel, 0}));
    }
};
PR_NO_PADDING_END

// A progress bar's track, which clips its run, or a meter's, whose cells are apart by one px.
PR_NO_PADDING_BEGIN
struct BarBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    bool clips;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.height = prLayoutLengthPx(kBarHeight);
        box.minWidth = prLayoutLengthPx(0);
        if (clips)
            box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
    }
};
struct BarFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    float gap;

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Stretch;
        if (gap > 0)
            flex.columnGap = prLayoutLengthPx(gap);
    }
};
PR_NO_PADDING_END

// A progress bar's run, or an indeterminate bar's band: `percent` of the track wide.
PR_NO_PADDING_BEGIN
struct RunBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float percent;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = prLayoutLengthPercent(percent);
    }
};
PR_NO_PADDING_END

// A run scaled from its start to `scale` of the track, or a band moved `offset` px along it. Each is a value that
// moves, which the effects group alone holds.
PR_NO_PADDING_BEGIN
struct RunEffects {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    float scale;
    float offset;

    void make(const PrKit&, PrLayoutEffectStyle& effects) const {
        effects.originX = prLayoutLengthPercent(0);
        if (scale != 1)
            effects.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{scale, 1}));
        if (offset != 0)
            effects.transform = prLayoutTransformFromAffine2(prAffine2Translate(PrVec2{offset, 0}));
    }
};
PR_NO_PADDING_END

// A meter's cell: a share of the meter that gives way to it.
struct CellBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.minWidth = prLayoutLengthPx(0);
    }
};

// A spinner's box: a square of `size` px.
PR_NO_PADDING_BEGIN
struct SpinnerBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = box.height = prLayoutLengthPx(size);
    }
};
PR_NO_PADDING_END

// A spinner turned to step `step` of kSpinSteps.
PR_NO_PADDING_BEGIN
struct SpinnerEffects {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    uint32_t step;

    void make(const PrKit&, PrLayoutEffectStyle& effects) const {
        effects.transform = prLayoutTransformFromAffine2(prAffine2Rotate(kTurn * float(step) / float(kSpinSteps)));
    }
};
PR_NO_PADDING_END

// The kind of a choice container. The kind decides which items the container takes.
enum : uint8_t {
    StripKind_Segmented,
    StripKind_Radios,
    StripKind_Tabs,
};
typedef uint8_t StripKind;

// The context that a choice container provides to its items for the current round. It holds the container's kind and
// style, the arrow keys that move between items, the most recently created item, and the number of items created so
// far. `focusNext` is true if the next item created takes focus. A tab strip also records whether its tabs can be
// dragged, the payload kind of a tab drag, its group, and the move that a drop made in the round, if `moved`.
struct Strip {
    PrKitChoiceStyle style;
    PrAppKey back;
    PrAppKey forward;
    PrLayoutNode previous;
    uint64_t payload;
    uint64_t group;
    KitTabs* PR_NULLABLE tabs;
    uint32_t count;
    StripKind kind;
    bool focusNext;
    bool reorders;
    bool moved;
    PrKitTabMove move;
};

// Provides a choice container's context on `node`, and returns it.
Strip& provideStrip(PrKit& kit, PrLayoutNode node, StripKind kind, PrKitChoiceStyle style) {
    Strip& strip = kitProvide<Strip>(kit, node, KitContext_Strip);
    bool column = kind == StripKind_Radios && style.direction != PrKitDirection_Row;
    strip.style = style;
    strip.back = column ? PrAppKey_Up : PrAppKey_Left;
    strip.forward = column ? PrAppKey_Down : PrAppKey_Right;
    strip.previous = kPrLayoutNodeNone;
    strip.kind = kind;
    return strip;
}

// Returns the context of the container of `kind` that an item created under `parent` belongs to. The item's parent must
// be the container itself.
Strip& stripOf(PrKit& kit, PrLayoutNode parent, StripKind kind) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    Strip* strip = kitContext<Strip>(kit, parent, KitContext_Strip, &provider);
    PR_ASSERT(strip != nullptr && provider == parent && strip->kind == kind);
    return *strip;
}

// Handles the arrow keys of a container's item, and counts the item. An arrow key pressed while the item has focus
// moves the focus to the neighboring item. The neighbor is activated in the next round, so the caller selects it as it
// would after a click.
void stripItem(PrKit& kit, Strip& strip, const KitControl& item) {
    PrUi* ui = kit.ui;
    if (strip.focusNext) {
        prUiSetFocus(ui, item.node);
        prUiActivate(ui, item.node);
        strip.focusNext = false;
    }
    if (!item.disabled) {
        if (prUiKeyPressed(ui, item.node, strip.back, 0) && strip.previous != kPrLayoutNodeNone) {
            prUiSetFocus(ui, strip.previous);
            prUiActivate(ui, strip.previous);
        }
        if (prUiKeyPressed(ui, item.node, strip.forward, 0))
            strip.focusNext = true;
    }
    strip.previous = item.node;
    ++strip.count;
}

// Returns true if a widget created under `parent` is disabled.
bool disabledUnder(PrKit& kit, PrLayoutNode parent) {
    return parent != kPrLayoutNodeNone && prUiIsDisabled(kit.ui, parent);
}

// Returns the look of a checkbox's box or a toggle's track, by whether it is on and whether it is disabled.
PrKitLookId checkLook(bool rail, bool on, bool disabled) {
    if (on)
        return disabled ? PrKitLookId_CheckOnDisabled : PrKitLookId_CheckOn;
    if (rail)
        return disabled ? PrKitLookId_RailDisabled : PrKitLookId_Rail;
    return disabled ? PrKitLookId_CheckDisabled : PrKitLookId_Check;
}

// Returns the record of a checkbox's, toggle's or radio's row of `size`.
PrLayoutStyleIndex rowStyle(PrKit& kit, PrKitControlSize size, bool disabled) {
    const PrKitPalette& p = kit.theme.palette;
    return kitStyle(kit, RowBox{.size = size}, RowFlex{}, choiceFont(PrKitTextRole_Body),
        KitInk{.color = disabled ? p.text4 : p.text1});
}

// Creates the row of a checkbox or a toggle, and flips its flag if the row is activated.
PrLayoutNode checkRow(PrKit* kit, PrLayoutNode parent, const PrKitCheck& check, PrKitControlSize size, bool rail) {
    PR_ASSERT(check.value != nullptr);
    PrKit& k = *kit;
    KitControl row = kitControl(k, parent, check.key, PrUiFlag_Pressable | PrUiFlag_Focusable, check.disabled);
    prLayoutTreeFrameSetStyle(k.tree, row.node, rowStyle(k, size, row.disabled));
    bool on = *check.value;
    // The glyph is drawn in the row's state, so that it fades as the pointer comes over the row.
    KitFill fill = kitDress(k, row, checkLook(rail, on, row.disabled)).fill;
    GlyphBox glyph{
        .width = rail ? kRailWidth : kGlyph,
        .height = kGlyph,
        .padding = rail ? 1.0f : 0.0f,
        .bordered = true,
    };
    PrLayoutNode box = prLayoutTreeFrameElement(k.tree, row.node, 0, kitStyle(k, glyph, GlyphFlex{.atStart = rail}, fill));
    if (!rail && on)
        prKitIcon(kit, box, {.icon = PrKitIconKind_Check}, {.size = 10, .tone = PrKitTone_Inverse});
    if (rail) {
        // The knob eases along the track by its transform, which composites without a layout, and holds still at either
        // end.
        float travel = prUiTween(k.ui, box, &kKitOwner, ChoiceStateKey_Knob, on ? 1.0f : 0.0f, kKnobSeconds, nullptr);
        const PrKitPalette& p = k.theme.palette;
        PrLayoutStyleIndex knob = kitStyle(k, GlyphBox{.width = kKnob, .height = kKnob}, GlyphFlex{},
            KitColors{.background = on ? p.accentText : p.text2}, KnobEffects{.travel = travel});
        prLayoutTreeFrameElement(k.tree, box, 0, knob);
    }
    if (check.label.size != 0)
        prLayoutTreeFrameText(k.tree, row.node, 0, check.label);
    if (prUiActivated(k.ui, row.node))
        kitEditOnce(k, row.node, [&] { *check.value = !*check.value; });
    return row.node;
}

// Returns the strip's payload kind. The kind is the strip itself, so that a tab drops only into its own strip.
uint64_t tabPayloadOf(PrLayoutNode strip) {
    return prHashMixKeys(strip, 0x7461627375ull);
}

// The key of a tab strip's caret. The caret is created before the tabs. Because it is keyed, the tabs keep matching
// their own nodes when the caret appears or disappears.
constexpr uint64_t kCaretKey = 0x6361726574ull;

// The retained state of a tab strip: the caret node of the previous round, or none.
struct TabStripState {
    PrLayoutNode caret;
};

// Returns the place of the tab `id` in the strip's order, or the number of tabs if the strip does not hold it.
uint32_t tabIndexOf(const KitTabs& tabs, uint64_t id) {
    uint32_t i = 0;
    while (i < tabs.order.count && tabs.order.data[i].id != id)
        ++i;
    return i;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrKitTabs : KitTabs {};

PrLayoutNode prKitSegmented(PrKit* kit, PrLayoutNode parent, PrKitChoices choices, PrKitChoiceStyle style) {
    PrKit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    PrLayoutStyleIndex record =
        kitStyle(k, TrackBox{.size = style.size}, TrackFlex{}, KitColors{.background = p.bg2, .border = p.line2});
    PrLayoutNode track = kitBoxNode(k, parent, PrKitBox{.key = choices.key, .disabled = choices.disabled},
        PrKitOverflow_Visible, record);
    provideStrip(k, track, StripKind_Segmented, style);
    return track;
}

PrLayoutNode prKitSegment(PrKit* kit, PrLayoutNode segmented, PrKitChoice choice) {
    PrKit& k = *kit;
    Strip& strip = stripOf(k, segmented, StripKind_Segmented);
    KitControl cell = kitControl(k, segmented, choice.key, PrUiFlag_Pressable | PrUiFlag_Focusable, choice.disabled);
    bool on = choice.selected;
    PrKitLookId look;
    if (cell.disabled)
        look = (on ? PrKitLookId_SegmentChosenDisabled : PrKitLookId_SegmentDisabled);
    else
        look = (on ? PrKitLookId_SegmentChosen : PrKitLookId_Segment);
    KitDress dress = kitDress(k, cell, look);
    prLayoutTreeFrameSetStyle(k.tree, cell.node,
        kitStyle(k, SegmentBox{}, SegmentFlex{}, choiceFont(PrKitTextRole_BodySmall), dress.fill, dress.ink));
    if (choice.icon != PrKitIconKind_None) {
        PrKitTone tone = cell.disabled ? PrKitTone_Faint : on ? PrKitTone_Primary : PrKitTone_Secondary;
        prKitIcon(kit, cell.node, {.icon = choice.icon}, {.size = 12, .tone = tone});
    }
    if (choice.label.size != 0)
        prLayoutTreeFrameText(k.tree, cell.node, 0, choice.label);
    stripItem(k, strip, cell);
    return cell.node;
}

PrLayoutNode prKitRadioGroup(PrKit* kit, PrLayoutNode parent, PrKitChoices choices, PrKitChoiceStyle style) {
    PrKit& k = *kit;
    PrLayoutNode group = kitBoxNode(k, parent, PrKitBox{.key = choices.key, .disabled = choices.disabled},
        PrKitOverflow_Visible, kitStyle(k, FlexBox{}, GroupFlex{.direction = style.direction}));
    provideStrip(k, group, StripKind_Radios, style);
    return group;
}

PrLayoutNode prKitRadio(PrKit* kit, PrLayoutNode group, PrKitChoice choice) {
    PrKit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    Strip& strip = stripOf(k, group, StripKind_Radios);
    KitControl option = kitControl(k, group, choice.key, PrUiFlag_Pressable | PrUiFlag_Focusable, choice.disabled);
    prLayoutTreeFrameSetStyle(k.tree, option.node, rowStyle(k, strip.style.size, option.disabled));
    bool on = choice.selected;
    PrKitLookId look;
    if (option.disabled)
        look = (on ? PrKitLookId_RadioOnDisabled : PrKitLookId_CheckDisabled);
    else
        look = (on ? PrKitLookId_RadioOn : PrKitLookId_Radio);
    PrLayoutStyleIndex ringRecord = kitStyle(k, GlyphBox{.width = kGlyph, .height = kGlyph, .bordered = true},
        GlyphFlex{}, kitDress(k, option, look).fill);
    PrLayoutNode ring = prLayoutTreeFrameElement(k.tree, option.node, 0, ringRecord);
    if (on) {
        PrColor mark = option.disabled ? p.bg4 : p.accent;
        prLayoutTreeFrameElement(k.tree, ring, 0,
            kitStyle(k, GlyphBox{.width = kPrKitSpace3, .height = kPrKitSpace3}, GlyphFlex{}, KitColors{.background = mark}));
    }
    if (choice.icon != PrKitIconKind_None)
        prKitIcon(kit, option.node, {.icon = choice.icon},
            {.size = 12, .tone = PrKitTone(option.disabled ? PrKitTone_Faint : PrKitTone_Secondary)});
    if (choice.label.size != 0)
        prLayoutTreeFrameText(k.tree, option.node, 0, choice.label);
    stripItem(k, strip, option);
    return option.node;
}

PrLayoutNode prKitCheckbox(PrKit* kit, PrLayoutNode parent, PrKitCheck check, PrKitChoiceStyle style) {
    return checkRow(kit, parent, check, style.size, false);
}

PrLayoutNode prKitToggle(PrKit* kit, PrLayoutNode parent, PrKitCheck check, PrKitChoiceStyle style) {
    return checkRow(kit, parent, check, style.size, true);
}

PrKitTabs* prKitTabsCreate(void) {
    return allocZeroed<PrKitTabs>();
}

void prKitTabsDestroy(PrKitTabs* PR_NULLABLE tabs) {
    if (tabs == nullptr)
        return;
    tabs->order.reset();
    prMemoryFree(tabs);
}

uint32_t prKitTabsCount(const PrKitTabs* tabs) {
    return tabs->order.count;
}

uint64_t prKitTabsAt(const PrKitTabs* tabs, uint32_t index) {
    return index < tabs->order.count ? tabs->order.data[index].id : 0;
}

uint32_t prKitTabsIndex(const PrKitTabs* tabs, uint64_t id) {
    return tabIndexOf(*tabs, id);
}

uint64_t prKitTabsSelected(const PrKitTabs* tabs) {
    return tabs->selected;
}

void prKitTabsInsert(PrKitTabs* tabs, uint64_t id, uint32_t index) {
    PR_ASSERT(id != 0);
    uint32_t count = tabs->order.count;
    PR_ASSERT(tabIndexOf(*tabs, id) == count);
    tabs->order.insert(index < count ? index : count, HeldTab{.id = id, .node = kPrLayoutNodeNone});
    if (tabs->selected == 0)
        tabs->selected = id;
}

void prKitTabsRemove(PrKitTabs* tabs, uint64_t id) {
    uint32_t at = tabIndexOf(*tabs, id);
    PR_ASSERT(at < tabs->order.count);
    tabs->order.erase(at);
    if (tabs->focusing == id)
        tabs->focusing = 0;
    if (tabs->selected != id)
        return;
    uint32_t count = tabs->order.count;
    tabs->selected = count == 0 ? 0 : tabs->order.data[at < count ? at : count - 1].id;
}

void prKitTabsMove(PrKitTabs* from, PrKitTabs* to, uint64_t id, uint32_t index) {
    prKitTabsRemove(from, id);
    prKitTabsInsert(to, id, index);
    to->selected = id;
}

void prKitTabsSelect(PrKitTabs* tabs, uint64_t id) {
    PR_ASSERT(tabIndexOf(*tabs, id) < tabs->order.count);
    tabs->selected = id;
}

uint64_t prKitTabsStep(PrKitTabs* tabs, int32_t step) {
    uint32_t count = tabs->order.count;
    if (count == 0)
        return 0;
    int64_t at = int64_t(tabIndexOf(*tabs, tabs->selected));
    int64_t to = ((at + step) % int64_t(count) + int64_t(count)) % int64_t(count);
    tabs->selected = tabs->order.data[to].id;
    return tabs->selected;
}

PrLayoutNode prKitTabStrip(PrKit* kit, PrLayoutNode parent, PrKitTabStrip strip, PrKitChoiceStyle style) {
    PR_ASSERT(strip.tabs != nullptr);
    PrKit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    PrUi* ui = k.ui;
    KitTabs& tabs = *strip.tabs;
    ++tabs.round;
    const PrUiPayload* dragging = prUiDragging(ui);
    bool groupOver = strip.group != 0 && dragging != nullptr && dragging->kind == strip.group;
    bool collapsed = strip.collapsesSingle && !groupOver && tabs.order.count <= 1;
    PrLayoutNode node =
        kitBoxNode(k, parent, PrKitBox{.key = strip.key, .disabled = strip.disabled}, PrKitOverflow_Visible,
            kitStyle(k, StripBox{.size = style.size, .collapsed = collapsed},
                KitColors{.background = p.bg1, .border = p.line2}));
    Strip& context = provideStrip(k, node, StripKind_Tabs, style);
    TabStripState& state = kitState<TabStripState>(k, node, ChoiceStateKey_TabStrip);
    context.tabs = &tabs;
    context.reorders = strip.reorderable && !disabledUnder(k, node);
    context.group = strip.group;
    context.payload = strip.group != 0 ? strip.group : tabPayloadOf(node);
    const PrUiPayload* carried = context.reorders ? dragging : nullptr;
    bool ours = carried != nullptr && carried->kind == context.payload;
    const PrUiPayload* dropped = context.reorders ? prUiDropped(ui, node) : nullptr;
    bool drop = dropped != nullptr && dropped->kind == context.payload;

    // A dragged tab goes before the first tab whose middle is past the pointer, in the strip's order, at the tabs' last
    // layout. The pointer is measured in each tab's own plane, and the caret is placed in the strip's content box from
    // the tabs' positions. Both stay correct when the strip is transformed or scrolled.
    PrVec2 pointer{};
    uint32_t slot = 0;
    float caret = 0;
    if ((ours || drop) && prUiPointer(ui, nullptr, &pointer)) {
        PrRect stripBox = prLayoutRectToPx(prLayoutNodeBounds(k.tree, node, PrLayoutCoordinates_Layout));
        bool found = false;
        for (uint32_t i = 0; i < tabs.order.count && !found; ++i) {
            PrLayoutNode tab = tabs.order.data[i].node;
            if (!prLayoutNodeIsValid(k.tree, tab))
                continue;
            PrRect box = prLayoutRectToPx(prLayoutNodeBounds(k.tree, tab, PrLayoutCoordinates_Layout));
            PrVec2 at{};
            if (prUiLocalPoint(ui, tab, pointer, &at) && at.x < (box.right - box.left) / 2) {
                slot = i;
                caret = box.left - stripBox.left;
                found = true;
            } else {
                caret = box.right - stripBox.left;
            }
        }
        if (!found)
            slot = tabs.order.count;
    }
    state.caret = kPrLayoutNodeNone;
    if (ours && prUiIsDropTarget(ui, node, context.payload)) {
        state.caret = prLayoutTreeFrameElement(k.tree, node, kCaretKey,
            kitStyle(k, CaretBox{.left = caret}, KitColors{.background = p.accent}));
    }
    uint32_t source = drop ? tabIndexOf(tabs, dropped->value) : tabs.order.count;
    if (drop && source < tabs.order.count) {
        // A tab moves within the strip. The slot counts the places between the tabs, and taking the tab out closes the
        // place that it left.
        uint32_t to = slot > source ? slot - 1 : slot;
        HeldTab moved = tabs.order.data[source];
        tabs.order.erase(source);
        tabs.order.insert(to, moved);
        tabs.selected = moved.id;
    } else if (drop && strip.group != 0) {
        context.moved = true;
        context.move = PrKitTabMove{.id = dropped->value, .kind = PrKitTabMoveKind_Received, .index = slot};
    }
    // A tab of the strip that the user dropped outside every window tears off, where the pointer let it go.
    PrAppDragOperation ended = PrAppDragOperation_None;
    const PrUiPayload* left = strip.group != 0 ? prUiExportEnded(ui, &ended) : nullptr;
    bool tornOff = left != nullptr && left->kind == context.payload && ended == PrAppDragOperation_None &&
        tabIndexOf(tabs, left->value) < tabs.order.count;
    if (tornOff) {
        PrAppDisplayPosition at{};
        PrAppWindow* window = prUiWindow(ui);
        if (window != nullptr)
            prAppPointerPosition(prAppWindowApp(window), &at);
        context.moved = true;
        context.move = PrKitTabMove{.id = left->value, .kind = PrKitTabMoveKind_TornOff, .position = at};
    }
    return node;
}

PrLayoutNode prKitTab(PrKit* kit, PrLayoutNode strip, PrKitTab tab) {
    PR_ASSERT(tab.id != 0);
    PrKit& k = *kit;
    PrUi* ui = k.ui;
    Strip& s = stripOf(k, strip, StripKind_Tabs);
    KitTabs& tabs = *s.tabs;
    uint32_t at = tabIndexOf(tabs, tab.id);
    PR_ASSERT(at < tabs.order.count);
    // A tab of a group has the same key in every window, so that it keeps its identity as it moves between strips.
    PrUiKey key = prUiKeyIndex(ui, strip, tab.id);
    if (s.group != 0)
        key = prUiKeyIndex(ui, kPrLayoutNodeNone, prHashMixKeys(s.group, tab.id));
    KitControl cell = kitControl(k, strip, key, PrUiFlag_Pressable | PrUiFlag_Focusable, tab.disabled);
    tabs.order.data[at].node = cell.node;
    tabs.order.data[at].round = tabs.round;
    if (prUiActivated(ui, cell.node))
        tabs.selected = tab.id;
    if (tabs.focusing == tab.id) {
        prUiSetFocus(ui, cell.node);
        tabs.focusing = 0;
    }
    bool on = tabs.selected == tab.id;
    PrKitLookId look;
    if (cell.disabled)
        look = (on ? PrKitLookId_TabChosenDisabled : PrKitLookId_TabDisabled);
    else
        look = (on ? PrKitLookId_TabChosen : PrKitLookId_Tab);
    KitDress dress = kitDress(k, cell, look);
    // The tab shows at its place in the strip's order, whatever order the round creates the tabs in.
    PrLayoutStyleIndex record = kitStyle(k, TabBox{.size = s.style.size}, TabFlex{.order = int32_t(at)},
        choiceFont(PrKitTextRole_Body), dress.fill, dress.ink);
    prLayoutTreeFrameSetStyle(k.tree, cell.node, record);
    // Left and Right select the neighbor in the strip's order, which takes focus. A neighbor that the round has not
    // created yet takes focus as the round creates it.
    if (!cell.disabled) {
        uint32_t neighbor = tabs.order.count;
        if (prUiKeyPressed(ui, cell.node, s.back, 0) && at > 0)
            neighbor = at - 1;
        if (prUiKeyPressed(ui, cell.node, s.forward, 0) && at + 1 < tabs.order.count)
            neighbor = at + 1;
        if (neighbor < tabs.order.count) {
            const HeldTab& next = tabs.order.data[neighbor];
            tabs.selected = next.id;
            if (next.node != kPrLayoutNodeNone && next.round == tabs.round)
                prUiSetFocus(ui, next.node);
            else
                tabs.focusing = next.id;
        }
    }
    // A tab of a group may leave the window for a strip of the group in another window.
    if (s.reorders && !cell.disabled && prUiIsPressed(ui, cell.node, 0)) {
        PrUiPayloadFlags flags = s.group != 0 ? PrUiPayloadFlags(PrUiPayloadFlag_AnyWindow) : PrUiPayloadFlags(0);
        prUiDragStart(ui, cell.node, kTabDragThreshold, s.payload, tab.id, flags);
    }
    s.previous = cell.node;
    ++s.count;
    return cell.node;
}

PrLayoutNode prKitTabLabel(PrKit* kit, PrLayoutNode tab, PrKitLabel label) {
    // The tab's style sets the font and the text color of its state, which the text takes.
    return prLayoutTreeFrameText(kit->tree, tab, label.key, label.text);
}

PrLayoutNode prKitTabClose(PrKit* kit, PrLayoutNode tab) {
    PrKit& k = *kit;
    KitControl close = kitControl(k, tab, 0, PrUiFlag_Pressable, false);
    PrLayoutStyleIndex record = kitStyle(k, GlyphBox{.width = kPrKitHeightXxs, .height = kPrKitHeightXxs}, GlyphFlex{},
        kitDress(k, close, PrKitLookId_Quiet).fill);
    prLayoutTreeFrameSetStyle(k.tree, close.node, record);
    PrKitTone tone = close.over ? PrKitTone_Primary : PrKitTone_Muted;
    prKitIcon(kit, close.node, {.icon = PrKitIconKind_Close}, {.size = 10, .tone = tone});
    return close.node;
}

bool prKitTabMoved(const PrKit* kit, PrLayoutNode strip, PrKitTabMove* out) {
    const Strip* s = kitContext<Strip>(*kit, strip, KitContext_Strip);
    *out = PrKitTabMove{};
    if (s == nullptr || !s->moved)
        return false;
    *out = s->move;
    return true;
}

PrLayoutNode prKitProgress(PrKit* kit, PrLayoutNode parent, PrKitProgress progress) {
    PrKit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    KitColors fill{.background = disabledUnder(k, parent) ? p.text4 : p.accent};
    PrLayoutStyleIndex barRecord = kitStyle(k, BarBox{.clips = true}, BarFlex{}, KitColors{.background = p.bg4});
    PrLayoutNode bar = prLayoutTreeFrameElement(k.tree, parent, progress.key, barRecord);
    prUiSetFlags(k.ui, bar, PrUiFlag_SubtreeTransparent);
    if (!progress.indeterminate) {
        // The run is scaled to the fraction, which composites without a layout.
        RunEffects run{.scale = unitClamp(progress.value)};
        prLayoutTreeFrameElement(k.tree, bar, 0, kitStyle(k, RunBox{.percent = 100}, KitFixed{}, fill, run));
        return bar;
    }
    // The band is moved across the track by its transform, from before the track's start to past its end, using the
    // track's width from the last layout.
    prUiRequestMotion(k.ui, bar);
    PrRect track = prLayoutRectToPx(prLayoutNodeBounds(k.tree, bar, PrLayoutCoordinates_Layout));
    float lead = phaseOf(prUiCurrentRound(k.ui)->time, kSweepSeconds) * (1 + kSweepShare) - kSweepShare;
    RunEffects band{.scale = 1, .offset = lead * (track.right - track.left)};
    prLayoutTreeFrameElement(k.tree, bar, 0, kitStyle(k, RunBox{.percent = kSweepShare * 100}, KitFixed{}, fill, band));
    return bar;
}

PrLayoutNode prKitMeter(PrKit* kit, PrLayoutNode parent, PrKitMeter meter, PrKitMeterStyle style) {
    PrKit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    bool disabled = disabledUnder(k, parent);
    float high = meter.low == 0 && meter.high == 0 ? 1 : meter.high;
    float span = high - meter.low;
    float fraction = span != 0 ? unitClamp((meter.value - meter.low) / span) : 0;
    uint32_t lit = uint32_t(fraction * float(kPrKitMeterSegments));
    PrLayoutNode bar = prLayoutTreeFrameElement(k.tree, parent, meter.key, kitStyle(k, BarBox{}, BarFlex{.gap = 1}));
    prUiSetFlags(k.ui, bar, PrUiFlag_SubtreeTransparent);
    PrColor on = disabled ? p.text4 : kitToneColor(k, style.tone);
    for (uint32_t i = 0; i < kPrKitMeterSegments; ++i) {
        KitColors cell{.background = i < lit ? on : p.bg4};
        prLayoutTreeFrameElement(k.tree, bar, 0, kitStyle(k, CellBox{}, KitGrow{}, cell));
    }
    return bar;
}

PrLayoutNode prKitSpinner(PrKit* kit, PrLayoutNode parent, PrKitSpinner spinner, PrKitSpinnerStyle style) {
    PrKit& k = *kit;
    float size = style.size > 0 ? style.size : kSpinnerSize;
    // The spinner turns by its transform, one step at a time, so that a turn composites without painting the icon
    // again, and each step's effects are kept.
    float phase = phaseOf(prUiCurrentRound(k.ui)->time, kSpinSeconds);
    uint32_t step = uint32_t(phase * float(kSpinSteps)) % kSpinSteps;
    PrKitTone tone = disabledUnder(k, parent) ? PrKitTone_Faint : PrKitTone_Accent;
    PrLayoutNode node = prKitIcon(kit, parent, {.key = spinner.key, .icon = PrKitIconKind_Spinner}, {.size = size, .tone = tone});
    prLayoutTreeFrameSetStyle(k.tree, node, kitStyle(k, SpinnerBox{.size = size}, KitFixed{}, SpinnerEffects{.step = step}));
    prUiRequestMotion(k.ui, node);
    return node;
}
