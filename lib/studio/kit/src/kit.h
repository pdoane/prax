// Studio Kit: the kit's state, and what its widgets share to make their records and answer their input.

#pragma once

#include "pr/array.h"
#include "pr/hash_index.h"
#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_icon.h"
#include "pr/kit_text.h"
#include "pr/layout_frame.h"
#include "pr/layout_style.h"
#include "pr/mem.h"
#include "pr/text_font.h"
#include "pr/ui_records.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Records
//
// A node's record is a group index of each kind (pr/layout_style.h). The kit makes a record from group recipes
// (pr/ui_records.h), never from a record's values. A part names the groups that it sets by their recipes, and each kind
// that it does not set is index 0, layout's default. A property that layout adds costs nothing to a part that does not
// set it, and a value that moves remakes only the group that holds it. The kit owns its recipes, which read its theme
// and fonts.
//
// kitGroup returns a recipe's group, and kitStyle the record of a part. Both come from the context's records, which
// every library on the context shares. kitStyle is the only way that a widget makes a record.

// The address that owns the kit's retained state on nodes (prUiState). It is one object, whichever file includes it.
inline constexpr char kKitOwner = 0;

// The keys of the kit's retained state on a node.
enum : uint64_t {
    KitStateKey_Hover = 1,
    KitStateKey_Press,

    // The first key of each widget header, which numbers its own keys from it, a hundred apart.
    KitStateKey_Menu = 100,
    KitStateKey_Choice = 200,
    KitStateKey_Collection = 300,
    KitStateKey_Input = 400,
    KitStateKey_Dock = 500,
    KitStateKey_Timeline = 700,
    KitStateKey_Graph = 800,
    KitStateKey_Color = 900,
};
typedef uint64_t KitStateKey;

// Returns the group of `recipe` (uiGroup).
template <typename Recipe>
PrLayoutGroupIndex kitGroup(PrKit& kit, const Recipe& recipe) {
    return uiGroup(kit.ui, kit, recipe);
}

// Returns the record of the part that `groups` make (uiStyle).
template <typename... Groups>
PrLayoutStyleIndex kitStyle(PrKit& kit, const Groups&... groups) {
    return uiStyle(kit.ui, kit, groups...);
}

// ---------------------------------------------------------------------------------------------------------------------
// Common groups
//
// The group recipes that many widgets share, and the writers of group values that recipes are made of.

// A block box: a box of layout's default but for its display.
struct KitBlock {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit& kit, PrLayoutBoxStyle& box) const;
};

// A block box that its parent may shrink to nothing in both axes.
struct KitShrinkable {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit& kit, PrLayoutBoxStyle& box) const;
};

// A flex item that does not shrink, and one that grows from nothing to take the room its parent leaves.
struct KitFixed {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit& kit, PrLayoutFlexStyle& flex) const;
};
struct KitGrow {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit& kit, PrLayoutFlexStyle& flex) const;
};

// The font of text in a role, wrapping as `wrap` says, aligned by `align`, and keeping its white space if `preserve` is
// set.
PR_NO_PADDING_BEGIN
struct KitFont {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Font;
    PrKitTextRole role;
    PrKitWrap wrap;
    PrKitTextAlign align;
    bool preserve;
    void make(const PrKit& kit, PrLayoutFontStyle& font) const;
};
PR_NO_PADDING_END

// The color of text.
PR_NO_PADDING_BEGIN
struct KitInk {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Color;
    PrColor color;
    void make(const PrKit& kit, PrLayoutColorStyle& color) const;
};
PR_NO_PADDING_END

// A decoration of a background and a border, solid on every side; the box decides the border's width.
PR_NO_PADDING_BEGIN
struct KitColors {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    PrColor background;
    PrColor border;
    void make(const PrKit& kit, PrLayoutDecorationStyle& decoration) const;
};
PR_NO_PADDING_END

// The color of text in a tone of the palette.
PR_NO_PADDING_BEGIN
struct KitToneInk {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Color;
    PrKitTone tone;
    void make(const PrKit& kit, PrLayoutColorStyle& color) const;
};
PR_NO_PADDING_END

// The box and the flex container of a box widget's style (pr/kit_box.h).
PR_NO_PADDING_BEGIN
struct KitContainerBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitBoxStyle style;
    void make(const PrKit& kit, PrLayoutBoxStyle& box) const;
};
struct KitContainerFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    PrKitDirection direction;
    PrKitJustify justify;
    PrKitAlign align;
    bool wrap;
    PrKitFlex flex;
    PrKitAlign alignSelf;
    uint8_t _pad[2];
    float gap;
    float weight;
    void make(const PrKit& kit, PrLayoutFlexStyle& flex) const;
};

// Returns the flex container of a box widget's style.
inline KitContainerFlex kitContainerFlex(const PrKitBoxStyle& style) {
    return KitContainerFlex{
        .direction = style.direction,
        .justify = style.justify,
        .align = style.align,
        .wrap = style.wrap,
        .flex = style.place.flex,
        .alignSelf = style.place.align,
        .gap = style.gap,
        .weight = style.place.weight,
    };
}
PR_NO_PADDING_END

// A box and a flex item placed by `place` over the groups `base` (prKitPlace).
PR_NO_PADDING_BEGIN
struct KitPlacedBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrLayoutGroupIndex base;
    PrKitPlace place;
    void make(const PrKit& kit, PrLayoutBoxStyle& box) const;
};
struct KitPlacedFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    PrLayoutGroupIndex base;
    PrKitPlace place;
    void make(const PrKit& kit, PrLayoutFlexStyle& flex) const;
};
PR_NO_PADDING_END

// Write a box's padding on every side, and its border's width; a decoration's border, solid on every side, in `color`;
// and the sizes, limits, margins, flex and alignment of `place`, in a box and a flex item.
void kitSetPadding(PrLayoutBoxStyle& box, PrKitEdges padding);
void kitSetBorderWidth(PrLayoutBoxStyle& box, float width);
void kitSetBorderColor(PrLayoutDecorationStyle& decoration, PrColor color);
void kitPlaceBox(PrLayoutBoxStyle& box, const PrKitPlace& place);
void kitPlaceFlex(PrLayoutFlexStyle& flex, const PrKitPlace& place);

// Makes a box under `parent` from its content and the record of its style, and gives it its flags: scrollable if its
// overflow scrolls, and disabled if its content says so. Every widget that is a box with a look of its own makes its
// node with this.
PrLayoutNode kitBoxNode(PrKit& kit, PrLayoutNode parent, PrKitBox box, PrKitOverflow overflow, PrLayoutStyleIndex style);

// ---------------------------------------------------------------------------------------------------------------------
// Controls
//
// A control is a node that takes input, and the state it is drawn in this round. A widget makes its node with
// kitControl, picks its look from the state, and passes the groups that kitDress gives it to kitStyle with its own.

// A control's node and its state in the round. `hover` and `press` ease between 0 and 1 as the pointer comes over the
// node and presses it, `over` and `down` are true while it does, and all are 0 while the control is disabled.
struct KitControl {
    PrLayoutNode node;
    float hover;
    float press;
    bool over;
    bool down;
    bool ring;                      // the node has focus that the keyboard put there, which the control shows
    bool disabled;                  // by its content, or by a container it was made under
};

// Makes a control under `parent`, keyed by `key`, that takes `flags` and is disabled if `disabled` is true or a
// container it is made under is disabled. A pressable control shows the hand cursor while it is enabled.
KitControl kitControl(PrKit& kit, PrLayoutNode parent, uint64_t key, PrUiFlags flags, bool disabled);

// The same, for a control on a node that the caller made.
KitControl kitControlFromNode(PrKit& kit, PrLayoutNode node, PrUiFlags flags, bool disabled);

// The decoration of a control drawn in `look`: its fill at `state`, or between states by the eased `hover` and `press`
// while it fades, the look's border on every side, and the focus ring if `ring` is set.
PR_NO_PADDING_BEGIN
struct KitFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    PrKitLookId look;
    PrKitState state;
    bool ring;
    float hover;
    float press;
    void make(const PrKit& kit, PrLayoutDecorationStyle& decoration) const;
};
PR_NO_PADDING_END

// The groups that a control's look gives its record: its fill, and its text's color.
struct KitDress {
    KitFill fill;
    KitInk ink;
};

// Returns the groups of a control drawn in `look` at its state this round. A look that does not ease takes each state
// at once; one that does is at a settled state, which its record is kept for, or fading between states.
KitDress kitDress(const PrKit& kit, const KitControl& control, PrKitLookId look);

// Returns the settled state of a control drawn in `look`, or false if the look eases and the control's hover or press
// is between states.
bool kitSettled(const PrKit& kit, const KitControl& control, PrKitLookId look, PrKitState* state);

// ---------------------------------------------------------------------------------------------------------------------
// Contexts and signals
//
// A container gives its items what they share through the tree, and a widget tells its caller what happened to it by a
// signal on its node. Both are lib/ui's, under the kit's owner.

// The contexts that the kit's containers provide.
enum : uint64_t {
    KitContext_ButtonGroup = 1,
    KitContext_Strip,
    KitContext_Closer,
    KitContext_Palette,
    KitContext_SelectMenu,
    KitContext_AssetDrop,
    KitContext_TextChange,
    KitContext_View,
    KitContext_Item,
    KitContext_Graph,
    KitContext_Timeline,
    KitContext_Split,
};
typedef uint64_t KitContext;

// Returns the kit's retained state of type `T` on `node` under `key` (prUiState). New state is zeroed.
template <typename T>
T& kitState(PrKit& kit, PrLayoutNode node, uint64_t key) {
    return *static_cast<T*>(prUiState(kit.ui, node, &kKitOwner, key, uint32_t(sizeof(T))));
}

// Returns the context of type `T` that `node` provides to the nodes made under it in the round, zeroed.
template <typename T>
T& kitProvide(PrKit& kit, PrLayoutNode node, uint64_t key) {
    return *static_cast<T*>(prUiProvide(kit.ui, node, &kKitOwner, key, uint32_t(sizeof(T))));
}

// The new text of a text control in the round, which prKitTextChanged reads.
struct KitTextChange {
    PrStringView text;
};

// Reports `text` as the new text of the control `node`.
inline void kitReportText(PrKit& kit, PrLayoutNode node, PrStringView text) {
    kitProvide<KitTextChange>(kit, node, KitContext_TextChange).text = text;
}

// Returns the context of type `T` that `node` or the nearest node it was made under provides, or null. Writes the
// provider to `provider` if it is not null.
template <typename T>
T* PR_NULLABLE kitContext(const PrKit& kit, PrLayoutNode node, uint64_t key, PrLayoutNode* PR_NULLABLE provider = nullptr) {
    return static_cast<T*>(prUiContext(kit.ui, node, &kKitOwner, key, provider));
}

// What can happen to a widget in a round, which its caller asks of its node.
enum {
    KitSignal_Removed = 1u << 0,
    KitSignal_StepBack = 1u << 1,
    KitSignal_StepForward = 1u << 2,
    KitSignal_Browsed = 1u << 3,
    KitSignal_Located = 1u << 4,
    KitSignal_Cleared = 1u << 5,
};
typedef uint32_t KitSignals;

// Marks `signals` on the widget's node, and returns true if any of them happened in the round.
void kitSignal(PrKit& kit, PrLayoutNode node, KitSignals signals);
bool kitSignaled(const PrKit& kit, PrLayoutNode node, KitSignals signals);

// Reports a whole edit gesture on `control` that makes one write (prUiEdit). `write` makes the write between the
// gesture's begin and its change.
template <typename Write>
void kitEditOnce(const PrKit& kit, PrLayoutNode control, Write write) {
    prUiEdit(kit.ui, control, PrUiEditPhase_Begin);
    write();
    prUiEdit(kit.ui, control, PrUiEditPhase_Change);
    prUiEdit(kit.ui, control, PrUiEditPhase_Commit);
}

// Sets the flags of a widget's node, and returns true if the node is disabled, by its own flags or by a container it
// was created under.
bool kitSetFlags(PrKit& kit, PrLayoutNode node, PrUiFlags flags);

// ---------------------------------------------------------------------------------------------------------------------
// Values

// Convert a kit size to a layout length, and a color to a group's encoding.
PrLayoutLength kitLength(PrKitSize size);
PrColor16 kitColor16(PrColor color);

// Returns `from` blended toward `to` by `t`, between 0 and 1. The colors have straight alpha, and blend as
// premultiplied colors do.
PrColor kitMix(PrColor from, PrColor to, float t);

// Returns the color of `tone` in the kit's palette. Auto is the primary tone.
PrColor kitToneColor(const PrKit& kit, PrKitTone tone);

// Return the tone and the wrap that text of `role` takes for `tone` and `wrap`: the role's own for Auto (PrKitType).
PrKitTone kitTextTone(const PrKit& kit, PrKitTextRole role, PrKitTone tone);
PrKitWrap kitTextWrap(const PrKit& kit, PrKitTextRole role, PrKitWrap wrap);

// Return the eased progress, between 0 and 1, of the pointer's hover over `node`, and of a press that the node holds.
float kitHoverFactor(PrKit& kit, PrLayoutNode node);
float kitPressFactor(PrKit& kit, PrLayoutNode node);

// Returns the extent in px of the node's border box at the last layout, before any transform: the size of the box in
// its own coordinates, which positions from prUiLocalPoint are measured in.
PrVec2 kitExtent(const PrKit& kit, PrLayoutNode node);

// Returns the height in px of a control of `size`.
float kitControlHeight(PrKitControlSize size);

} // namespace pr_impl

// C opaque types
PR_ASSUME_NONNULL_END
