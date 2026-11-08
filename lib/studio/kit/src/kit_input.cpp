// Studio Kit's sliders, numeric fields, vectors, colors, and the inspector.

#include "pr/kit_input.h"

#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/integer.h"
#include "pr/kit_box.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/raw_array.h"
#include "pr/ui_text_edit.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Records

// The parts the family's widgets are made of, each a family of its own.
enum : uint8_t {
    Part_NumberFrame,
    Part_NumberAxis,
    Part_NumberValue,
    Part_NumberUnit,
    Part_Vector,
    Part_SliderFrame,
    Part_SliderTrack,
    Part_GroupFrame,
    Part_GroupHeader,
    Part_GroupBody,
    Part_RowFrame,
    Part_RowLabelCell,
    Part_RowLabel,
    Part_RowControls,
    Part_AssetFrame,
    Part_AssetThumb,
    Part_AssetName,
    Part_AssetGutter,
    Part_AssetAction,
    Part_TextFrame,
    Part_TextValue,
    Part_TextPlaceholder,
    Part_TextClear,
    Part_AreaFrame,
    Part_AreaValue,
};
typedef uint8_t Part;

// The state that a part is drawn in: at rest, hovered, pressed through, or active (focused, scrubbing, or under a
// drop).
enum : uint8_t {
    PartState_Rest,
    PartState_Hover,
    PartState_Press,
    PartState_Active,
};
typedef uint8_t PartState;

// The conditions a part's record depends on.
enum : uint8_t {
    PartFlag_Disabled = 1u << 0,
    PartFlag_InRow = 1u << 1,           // its parent is a row, along which it fills the remaining room
    PartFlag_Ring = 1u << 2,            // shows the focus ring
    PartFlag_Mono = 1u << 3,
    PartFlag_Missing = 1u << 4,
    PartFlag_Bare = 1u << 5,            // a text field with no frame of its own
    PartFlag_Wraps = 1u << 6,           // text that wraps, as in a text area
};
typedef uint8_t PartFlags;

PR_NO_PADDING_BEGIN
struct PartRecipe {
    uint8_t part;
    PrKitControlSize size;
    uint8_t state;
    PrKitTone tone;
    uint8_t flags;
};
PR_NO_PADDING_END

// The keys of the retained state of the family's widgets.
enum : uint64_t {
    StateKey_Number = KitStateKey_Input,
    StateKey_Slider,
    StateKey_Group,
    StateKey_Text,
};
typedef uint64_t StateKey;

// The payload value that was dropped on an asset field in the round, which the field provides to prKitDropped.
struct AssetDrop {
    uint64_t value;
};

// The distance in px that a press travels before a numeric field reads it as a scrub.
constexpr float kScrubThreshold = 3;

// The change for each px that a scrub drags if the range names none, and the factor by which Shift scales it.
constexpr double kDefaultStep = 0.05;
constexpr double kFineFactor = 0.1;

// The number of steps that a key press moves a numeric field, without and with Shift.
constexpr double kKeySteps = 10;
constexpr double kFineKeySteps = 1;

// The number of fraction digits that a float shows if the field names none.
constexpr uint32_t kDefaultPrecision = 2;

// The width of an inspector row's label column, in px.
constexpr float kLabelWidth = 88;

// The vertical padding of a text area, in px.
constexpr float kAreaPadding = 4;

// A slider's track height and its knob's extent, in px.
constexpr float kTrackHeight = 4;
constexpr float kKnobWidth = 6;
constexpr float kKnobHeight = 12;

// Returns the role that a text control sets its text in: monospaced, or the body role of its size.
PrKitTextRole textRole(const PartRecipe& recipe) {
    if ((recipe.flags & PartFlag_Mono) != 0)
        return PrKitTextRole_Mono;
    bool small = recipe.size == PrKitControlSize_Xs || recipe.size == PrKitControlSize_Sm;
    return small ? PrKitTextRole_BodySmall : PrKitTextRole_Body;
}

// Writes a flex row's box: a border box that lays out its children in a row.
void rowBox(PrLayoutBoxStyle& box) {
    box.display = PrLayoutDisplay_Flex;
    box.boxSizing = PrLayoutBoxSizing_BorderBox;
}

// Writes a part's box and flex so that it fills the room that its row leaves, or keeps its own size in a column.
void fillRowBox(PrLayoutBoxStyle& box, bool inRow) {
    if (inRow)
        box.minWidth = prLayoutLengthPx(0);
}
void fillRowFlex(PrLayoutFlexStyle& flex, bool inRow) {
    if (inRow) {
        flex.grow = 1;
        flex.shrink = 1;
        flex.basis = prLayoutLengthPx(0);
    } else {
        flex.shrink = 0;
    }
}

// Writes a decoration's rule on one side, in `color`, and the focus ring.
void setRule(PrLayoutDecorationStyle& decoration, PrRectSide side, PrColor color) {
    decoration.borderColor[side] = kitColor16(color);
    decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
}
void setRing(PrLayoutDecorationStyle& decoration, const PrKit& kit) {
    decoration.outlineColor = kitColor16(kit.theme.palette.accent);
    decoration.outlineStyle = PrDrawBorderStyle_Solid;
    decoration.outlineWidth = prLayoutUnitFromPx(1);
    decoration.outlineOffset = prLayoutUnitFromPx(1);
}

// The groups of a part, each written from the part's recipe.
PR_NO_PADDING_BEGIN
struct InputBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PartRecipe recipe;

    void make(const PrKit& kit, PrLayoutBoxStyle& box) const {
        bool inRow = (recipe.flags & PartFlag_InRow) != 0;
        float height = kitControlHeight(recipe.size);
        box.display = PrLayoutDisplay_Block;
        switch (recipe.part) {
            case Part_NumberFrame:
                rowBox(box);
                box.height = prLayoutLengthPx(height);
                box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
                kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace3));
                kitSetBorderWidth(box, 1);
                fillRowBox(box, inRow);
                break;
            case Part_NumberValue:
                box.minWidth = prLayoutLengthPx(0);
                box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
                break;
            case Part_Vector:
                rowBox(box);
                fillRowBox(box, inRow);
                break;
            case Part_SliderFrame:
                rowBox(box);
                box.height = prLayoutLengthPx(height);
                // The padding holds the knob's halves at the track's ends.
                kitSetPadding(box, prKitEdgesXY(0, kKnobWidth / 2));
                box.minWidth = prLayoutLengthPx(kPrKitSpace16);
                break;
            case Part_SliderTrack:
                box.position = PrLayoutPosition_Relative;
                box.height = prLayoutLengthPx(kTrackHeight);
                box.minWidth = prLayoutLengthPx(0);
                break;
            case Part_GroupFrame:
                box.display = PrLayoutDisplay_Flex;
                box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(1);
                break;
            case Part_GroupHeader:
                rowBox(box);
                box.height = prLayoutLengthPx(kPrKitHeightSm);
                kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace3));
                box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(1);
                break;
            case Part_GroupBody:
                box.display = PrLayoutDisplay_Flex;
                break;
            case Part_RowFrame:
                rowBox(box);
                box.minHeight = prLayoutLengthPx(kPrKitHeightMd);
                kitSetPadding(box, PrKitEdges{kPrKitSpace1, kPrKitSpace3, kPrKitSpace1, kPrKitSpace6});
                break;
            case Part_RowLabelCell:
                rowBox(box);
                box.width = prLayoutLengthPx(kLabelWidth);
                box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
                break;
            case Part_RowControls:
                rowBox(box);
                fillRowBox(box, true);
                break;
            case Part_AssetFrame:
                rowBox(box);
                box.height = prLayoutLengthPx(kPrKitHeightSm);
                box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
                kitSetBorderWidth(box, 1);
                fillRowBox(box, inRow);
                break;
            case Part_AssetThumb:
                rowBox(box);
                box.width = prLayoutLengthPx(kPrKitHeightXs);
                box.border[PrRectSide_Right] = prLayoutUnitFromPx(1);
                break;
            case Part_AssetName:
                rowBox(box);
                box.minWidth = prLayoutLengthPx(0);
                box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
                kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace3));
                break;
            case Part_AssetGutter:
                rowBox(box);
                break;
            case Part_AssetAction:
                rowBox(box);
                box.width = prLayoutLengthPx(kPrKitHeightXs);
                box.border[PrRectSide_Left] = prLayoutUnitFromPx(1);
                break;
            case Part_TextFrame:
            case Part_AreaFrame:
                rowBox(box);
                if (recipe.part == Part_AreaFrame) {
                    // The tone holds the area's rows, which give its height at the line height of its role.
                    float rows = float(recipe.tone != 0 ? recipe.tone : 3);
                    box.height = prLayoutLengthPx(rows * kit.theme.types[textRole(recipe)].lineHeight + 2 * kAreaPadding + 2);
                    box.overflowX = PrLayoutOverflow_Hidden;
                    box.overflowY = PrLayoutOverflow_Scroll;
                    kitSetPadding(box, prKitEdgesXY(kAreaPadding, kPrKitSpace3));
                } else {
                    box.height = prLayoutLengthPx(height);
                    box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
                    kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace3));
                }
                if ((recipe.flags & PartFlag_Bare) == 0)
                    kitSetBorderWidth(box, 1);
                else
                    kitSetPadding(box, prKitEdgesAll(0));
                fillRowBox(box, inRow);
                break;
            case Part_TextValue:
                // The value scrolls along its line to keep the caret in view, which only the field does.
                box.minWidth = prLayoutLengthPx(0);
                box.overflowX = box.overflowY = PrLayoutOverflow_Hidden;
                break;
            case Part_AreaValue:
                box.minWidth = prLayoutLengthPx(0);
                break;
            case Part_TextPlaceholder:
                box.display = PrLayoutDisplay_Inline;
                break;
            case Part_TextClear:
                rowBox(box);
                box.width = box.height = prLayoutLengthPx(kPrKitHeightXxs);
                break;
            default:
                break;
        }
    }
};

struct InputFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    PartRecipe recipe;

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        bool inRow = (recipe.flags & PartFlag_InRow) != 0;
        // A flex row centers its children across it, apart by its gap.
        auto row = [&](float gap) {
            flex.alignItems = PrLayoutAlignment_Center;
            flex.columnGap = prLayoutLengthPx(gap);
        };
        switch (recipe.part) {
            case Part_NumberFrame:
                row(kPrKitSpace2);
                fillRowFlex(flex, inRow);
                break;
            case Part_NumberAxis:
            case Part_NumberUnit:
                flex.shrink = 0;
                break;
            case Part_NumberValue:
                flex.grow = 1;
                break;
            case Part_Vector:
                row(kPrKitSpace2);
                fillRowFlex(flex, inRow);
                break;
            case Part_SliderFrame:
                row(0);
                fillRowFlex(flex, inRow);
                break;
            case Part_SliderTrack:
                flex.grow = 1;
                flex.basis = prLayoutLengthPx(0);
                break;
            case Part_GroupFrame:
            case Part_GroupBody:
                flex.direction = PrLayoutFlexDirection_Column;
                flex.shrink = 0;
                break;
            case Part_GroupHeader:
                row(kPrKitSpace2);
                flex.shrink = 0;
                break;
            case Part_RowFrame:
                row(kPrKitSpace3);
                flex.shrink = 0;
                break;
            case Part_RowLabelCell:
                row(0);
                flex.shrink = 0;
                break;
            case Part_RowControls:
                row(kPrKitSpace2);
                fillRowFlex(flex, true);
                break;
            case Part_AssetFrame:
                row(0);
                flex.alignItems = PrLayoutAlignment_Stretch;
                fillRowFlex(flex, inRow);
                break;
            case Part_AssetThumb:
            case Part_AssetAction:
            case Part_TextClear:
                row(0);
                flex.shrink = 0;
                flex.justifyContent = PrLayoutAlignment_Center;
                break;
            case Part_AssetName:
                row(0);
                flex.grow = 1;
                flex.basis = prLayoutLengthPx(0);
                break;
            case Part_AssetGutter:
                row(0);
                flex.shrink = 0;
                flex.alignItems = PrLayoutAlignment_Stretch;
                break;
            case Part_TextFrame:
            case Part_AreaFrame:
                row(kPrKitSpace2);
                if (recipe.part == Part_AreaFrame)
                    flex.alignItems = PrLayoutAlignment_FlexStart;
                fillRowFlex(flex, inRow);
                break;
            case Part_TextValue:
            case Part_AreaValue:
                flex.grow = 1;
                flex.basis = prLayoutLengthPx(0);
                break;
            default:
                break;
        }
    }
};

struct InputFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    PartRecipe recipe;

    void make(const PrKit& kit, PrLayoutDecorationStyle& decoration) const {
        const PrKitPalette& p = kit.theme.palette;
        const PrColor clear{0, 0, 0, 0};
        bool disabled = (recipe.flags & PartFlag_Disabled) != 0;
        bool ring = (recipe.flags & PartFlag_Ring) != 0;
        // A frame's border is the accent while it is active, and brighter while the pointer is over it.
        PrColor frameBorder = recipe.state == PartState_Active ? p.accent : recipe.state == PartState_Rest ? p.line2 : p.line3;
        const PrColor quiet[] = {clear, p.bg3, p.bg4, p.bg3};
        switch (recipe.part) {
            case Part_NumberFrame:
                kitSetBorderColor(decoration, disabled ? p.line1 : frameBorder);
                decoration.background = kitColor16(disabled ? p.bg1 : p.bg2);
                break;
            case Part_SliderFrame:
                if (ring)
                    setRing(decoration, kit);
                break;
            case Part_SliderTrack:
                decoration.background = kitColor16(p.bg4);
                break;
            case Part_GroupFrame:
                setRule(decoration, PrRectSide_Bottom, p.line1);
                break;
            case Part_GroupHeader: {
                const PrColor fills[] = {p.bg2, p.bg3, p.bg4, p.bg3};
                decoration.background = kitColor16(fills[recipe.state]);
                setRule(decoration, PrRectSide_Bottom, p.line1);
                if (ring)
                    setRing(decoration, kit);
                break;
            }
            case Part_RowFrame:
                decoration.background = kitColor16(recipe.state == PartState_Hover ? p.bg2 : clear);
                break;
            case Part_AssetFrame: {
                bool drop = recipe.state == PartState_Active;
                if (drop)
                    kitSetBorderColor(decoration, p.accent);
                else if ((recipe.flags & PartFlag_Missing) != 0)
                    kitSetBorderColor(decoration, p.danger);
                else
                    kitSetBorderColor(decoration, p.line2);
                decoration.background = kitColor16(drop ? p.accentWeak : p.bg2);
                break;
            }
            case Part_AssetThumb:
                decoration.background = kitColor16(p.bg3);
                setRule(decoration, PrRectSide_Right, p.line1);
                break;
            case Part_AssetAction:
                decoration.background = kitColor16(quiet[recipe.state]);
                setRule(decoration, PrRectSide_Left, p.line1);
                if (ring)
                    setRing(decoration, kit);
                break;
            case Part_TextFrame:
            case Part_AreaFrame:
                if ((recipe.flags & PartFlag_Bare) == 0) {
                    kitSetBorderColor(decoration, disabled ? p.line1 : frameBorder);
                    decoration.background = kitColor16(disabled ? p.bg1 : p.bg2);
                    if (ring)
                        setRing(decoration, kit);
                }
                break;
            case Part_TextClear:
                decoration.background = kitColor16(quiet[recipe.state]);
                break;
            default:
                break;
        }
    }
};
PR_NO_PADDING_END

// A slider's run and knob, which stand at the value's `percent` of the track. The value can be any fraction, and moves
// only these box groups.
PR_NO_PADDING_BEGIN
struct SliderRunBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float percent;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = prLayoutLengthPercent(percent);
        box.height = prLayoutLengthPercent(100);
    }
};
struct SliderKnobBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float percent;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPercent(percent);
        box.inset[PrRectSide_Top] = prLayoutLengthPx((kTrackHeight - kKnobHeight) / 2);
        box.margin[PrRectSide_Left] = prLayoutLengthPx(-kKnobWidth / 2);
        box.width = prLayoutLengthPx(kKnobWidth);
        box.height = prLayoutLengthPx(kKnobHeight);
    }
};

// An asset's thumbnail tinted in `tint`, which is content and can be any color, with the thumbnail's rule.
struct TintFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    PrColor tint;

    void make(const PrKit& kit, PrLayoutDecorationStyle& decoration) const {
        decoration.background = kitColor16(tint);
        setRule(decoration, PrRectSide_Right, kit.theme.palette.line1);
    }
};
PR_NO_PADDING_END

// Returns true if a part sets text of its own, and writes its font and ink if so.
bool partText(const PrKit& kit, const PartRecipe& recipe, KitFont* font, KitInk* ink) {
    const PrKitPalette& p = kit.theme.palette;
    bool disabled = (recipe.flags & PartFlag_Disabled) != 0;
    PrKitTextRole role = PrKitTextRole_Body;
    PrColor color = p.text1;
    switch (recipe.part) {
        case Part_NumberAxis:
            role = PrKitTextRole_MonoStrong;
            color = kitToneColor(kit, recipe.tone);
            break;
        case Part_NumberValue:
            role = PrKitTextRole_Mono;
            color = disabled ? p.text4 : p.text1;
            break;
        case Part_NumberUnit:
            role = PrKitTextRole_Meta;
            color = p.text4;
            break;
        case Part_RowLabel:
            role = (recipe.flags & PartFlag_Mono) != 0 ? PrKitTextRole_Mono : PrKitTextRole_BodySmall;
            color = p.text3;
            break;
        case Part_AssetName:
            role = PrKitTextRole_BodySmall;
            color = kitToneColor(kit, recipe.tone);
            break;
        case Part_TextValue:
        case Part_TextPlaceholder:
        case Part_AreaValue:
            role = textRole(recipe);
            color = recipe.part == Part_TextPlaceholder || disabled ? p.text4 : p.text1;
            break;
        default:
            return false;
    }
    // The text of a field or an area keeps its white space, so that each typed space shows.
    bool editable =
        recipe.part == Part_TextValue || recipe.part == Part_TextPlaceholder || recipe.part == Part_AreaValue;
    bool wraps = (recipe.flags & PartFlag_Wraps) != 0;
    *font = KitFont{.role = role, .wrap = PrKitWrap(wraps ? PrKitWrap_Wrap : PrKitWrap_Nowrap), .preserve = editable};
    *ink = KitInk{.color = color};
    return true;
}

// Returns the record of a part.
PrLayoutStyleIndex partStyle(PrKit& kit, PartRecipe recipe) {
    KitFont font{};
    KitInk ink{};
    if (partText(kit, recipe, &font, &ink))
        return kitStyle(kit, InputBox{recipe}, InputFlex{recipe}, InputFill{recipe}, font, ink);
    return kitStyle(kit, InputBox{recipe}, InputFlex{recipe}, InputFill{recipe});
}

// Creates an element with a part's record under `parent`.
PrLayoutNode part(PrKit& kit, PrLayoutNode parent, uint64_t key, PartRecipe recipe) {
    return prLayoutTreeFrameElement(kit.tree, parent, key, partStyle(kit, recipe));
}

// Creates an element with a part's record that holds `text`.
PrLayoutNode textPart(PrKit& kit, PrLayoutNode parent, uint64_t key, PartRecipe recipe, PrStringView text) {
    PrLayoutNode node = part(kit, parent, key, recipe);
    prLayoutTreeFrameText(kit.tree, node, 0, text);
    return node;
}

// Returns true if `parent`, which is a node that the round created, lays out its children in a row.
bool isRow(const PrKit& kit, PrLayoutNode parent) {
    PrLayoutStyle style = prLayoutStyleRecord(kit.tree, prLayoutTreeFrameStyle(kit.tree, parent));
    PrLayoutBoxStyle box = prLayoutBoxStyleRecord(kit.tree, style.groups[PrLayoutGroup_Box]);
    if (box.display != PrLayoutDisplay_Flex && box.display != PrLayoutDisplay_InlineFlex)
        return false;
    PrLayoutFlexStyle flex = prLayoutFlexStyleRecord(kit.tree, style.groups[PrLayoutGroup_Flex]);
    return flex.direction == PrLayoutFlexDirection_Row || flex.direction == PrLayoutFlexDirection_RowReverse;
}

// Returns the state that a pressable node is drawn in, from the round's input.
uint8_t pointerState(const PrKit& kit, PrLayoutNode node, bool disabled) {
    if (disabled)
        return PartState_Rest;
    if (prUiIsPressed(kit.ui, node, 0))
        return PartState_Press;
    return prUiIsHovered(kit.ui, node) ? PartState_Hover : PartState_Rest;
}

bool ringed(const PrKit& kit, PrLayoutNode node) {
    return prUiIsFocusVisible(kit.ui) && prUiFocus(kit.ui) == node;
}

// Returns `flags`, with PrUiFlag_Disabled if `disabled` is true.
PrUiFlags editFlags(PrUiFlags flags, bool disabled) {
    return disabled ? flags | PrUiFlag_Disabled : flags;
}

// Creates an element under `parent` that takes `flags`, and writes whether it is disabled to `disabled`.
PrLayoutNode control(PrKit& kit, PrLayoutNode parent, uint64_t key, PrUiFlags flags, bool* disabled) {
    PrLayoutNode node = prLayoutTreeFrameElement(kit.tree, parent, key, 0);
    *disabled = kitSetFlags(kit, node, flags);
    return node;
}

// ---------------------------------------------------------------------------------------------------------------------
// Edit gestures

// Begins a gesture on `node` if none is open, as recorded by `live`.
void beginEdit(const PrKit& kit, PrLayoutNode node, bool* live) {
    if (*live)
        return;
    *live = true;
    prUiEdit(kit.ui, node, PrUiEditPhase_Begin);
}

// Commits the gesture open on `node`, if any.
void commitEdit(const PrKit& kit, PrLayoutNode node, bool* live) {
    if (!*live)
        return;
    *live = false;
    prUiEdit(kit.ui, node, PrUiEditPhase_Commit);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scalars

bool isIntegral(PrKitScalarKind kind) {
    return kind != PrKitScalarKind_F32 && kind != PrKitScalarKind_F64;
}

uint32_t scalarSize(PrKitScalarKind kind) {
    switch (kind) {
        case PrKitScalarKind_I8:
        case PrKitScalarKind_U8:
            return 1;
        case PrKitScalarKind_I16:
        case PrKitScalarKind_U16:
            return 2;
        case PrKitScalarKind_F32:
        case PrKitScalarKind_I32:
        case PrKitScalarKind_U32:
            return 4;
        default:
            return 8;
    }
}

// Writes the range of values that the kind holds. A 64-bit integer's bounds are the doubles below its limits.
void scalarBounds(PrKitScalarKind kind, double* low, double* high) {
    switch (kind) {
        case PrKitScalarKind_F32:
            *low = -3.4028234663852886e38;
            *high = 3.4028234663852886e38;
            return;
        case PrKitScalarKind_I8:
            *low = -128.0;
            *high = 127.0;
            return;
        case PrKitScalarKind_I16:
            *low = -32768.0;
            *high = 32767.0;
            return;
        case PrKitScalarKind_I32:
            *low = -2147483648.0;
            *high = 2147483647.0;
            return;
        case PrKitScalarKind_I64:
            *low = -9223372036854775808.0;
            *high = 9223372036854774784.0;
            return;
        case PrKitScalarKind_U8:
            *low = 0;
            *high = 255.0;
            return;
        case PrKitScalarKind_U16:
            *low = 0;
            *high = 65535.0;
            return;
        case PrKitScalarKind_U32:
            *low = 0;
            *high = 4294967295.0;
            return;
        case PrKitScalarKind_U64:
            *low = 0;
            *high = 18446744073709549568.0;
            return;
        default:
            *low = -1.7976931348623157e308;
            *high = 1.7976931348623157e308;
            return;
    }
}

double clampF64(double value, double low, double high) {
    return value < low ? low : value > high ? high : value;
}

double scalarLoad(PrKitScalar s) {
    switch (s.kind) {
        case PrKitScalarKind_F32:
            return double(*static_cast<const float*>(s.at));
        case PrKitScalarKind_F64:
            return *static_cast<const double*>(s.at);
        case PrKitScalarKind_I8:
            return double(*static_cast<const int8_t*>(s.at));
        case PrKitScalarKind_I16:
            return double(*static_cast<const int16_t*>(s.at));
        case PrKitScalarKind_I32:
            return double(*static_cast<const int32_t*>(s.at));
        case PrKitScalarKind_I64:
            return double(*static_cast<const int64_t*>(s.at));
        case PrKitScalarKind_U8:
            return double(*static_cast<const uint8_t*>(s.at));
        case PrKitScalarKind_U16:
            return double(*static_cast<const uint16_t*>(s.at));
        case PrKitScalarKind_U32:
            return double(*static_cast<const uint32_t*>(s.at));
        default:
            return double(*static_cast<const uint64_t*>(s.at));
    }
}

// Returns `value` as the kind holds it: finite, within the kind's bounds, and whole for an integer.
double scalarQuantize(PrKitScalarKind kind, double value) {
    double low = 0;
    double high = 0;
    scalarBounds(kind, &low, &high);
    double held = clampF64(prF64IsFinite(value) ? value : 0, low, high);
    if (kind == PrKitScalarKind_F32)
        return double(float(held));
    return isIntegral(kind) ? prF64Round(held) : held;
}

void scalarStore(PrKitScalar s, double value) {
    double held = scalarQuantize(s.kind, value);
    switch (s.kind) {
        case PrKitScalarKind_F32:
            *static_cast<float*>(s.at) = float(held);
            return;
        case PrKitScalarKind_F64:
            *static_cast<double*>(s.at) = held;
            return;
        case PrKitScalarKind_I8:
            *static_cast<int8_t*>(s.at) = int8_t(held);
            return;
        case PrKitScalarKind_I16:
            *static_cast<int16_t*>(s.at) = int16_t(held);
            return;
        case PrKitScalarKind_I32:
            *static_cast<int32_t*>(s.at) = int32_t(held);
            return;
        case PrKitScalarKind_I64:
            *static_cast<int64_t*>(s.at) = int64_t(held);
            return;
        case PrKitScalarKind_U8:
            *static_cast<uint8_t*>(s.at) = uint8_t(held);
            return;
        case PrKitScalarKind_U16:
            *static_cast<uint16_t*>(s.at) = uint16_t(held);
            return;
        case PrKitScalarKind_U32:
            *static_cast<uint32_t*>(s.at) = uint32_t(held);
            return;
        default:
            *static_cast<uint64_t*>(s.at) = uint64_t(held);
            return;
    }
}

PrKitScalar scalarAt(PrKitScalar s, uint32_t index) {
    return PrKitScalar{static_cast<uint8_t*>(s.at) + index * scalarSize(s.kind), s.kind};
}

// Writes `value` if it changes what the binding holds, and reports the change.
void scalarWrite(const PrKit& kit, PrLayoutNode node, PrKitScalar s, double value) {
    if (scalarQuantize(s.kind, value) == scalarLoad(s))
        return;
    scalarStore(s, value);
    prUiEdit(kit.ui, node, PrUiEditPhase_Change);
}

// A range's step and bounds, resolved against the bounds of its kind.
struct NumberBounds {
    double step;
    double min;
    double max;
};

NumberBounds boundsOf(const PrKitNumberRange& range, PrKitScalarKind kind) {
    NumberBounds b{.step = range.step != 0 ? range.step : kDefaultStep};
    scalarBounds(kind, &b.min, &b.max);
    if (range.hasMin && range.min > b.min)
        b.min = range.min;
    if (range.hasMax && range.max < b.max)
        b.max = range.max;
    return b;
}

// Writes `value` to `out`, which holds 64 bytes, with `precision` fraction digits. If the value is too large for that,
// writes its shortest digits and an exponent instead. Returns the number of bytes written.
uint32_t formatNumber(double value, uint32_t precision, char* out) {
    if (!prF64IsFinite(value))
        value = 0;
    double magnitude = value < 0 ? -value : value;
    precision = precision > 9 ? 9 : precision;
    double scale = 1;
    for (uint32_t i = 0; i < precision; ++i)
        scale *= 10;
    uint32_t size = 0;
    if (magnitude * scale >= 1e18) {
        PrF64Decimal d = prF64ShortestDecimal(value);
        if (value < 0)
            out[size++] = '-';
        size += uint32_t(prU64Format(d.digits, out + size, 10));
        out[size++] = 'e';
        int32_t exponent = d.exponent;
        if (exponent < 0) {
            out[size++] = '-';
            exponent = -exponent;
        }
        size += uint32_t(prU64Format(uint64_t(exponent), out + size, 10));
        return size;
    }
    uint64_t scaled = uint64_t(magnitude * scale + 0.5);
    char digits[kPrU64TextMaxBytes];
    uint32_t count = uint32_t(prU64Format(scaled, digits, 10));
    if (value < 0 && scaled != 0)
        out[size++] = '-';
    // The digits are padded with leading zeros to one more than the number of fraction digits, so that a whole digit
    // leads.
    uint32_t zeros = count <= precision ? precision + 1 - count : 0;
    uint32_t total = zeros + count;
    for (uint32_t i = 0; i < total; ++i) {
        if (precision != 0 && i == total - precision)
            out[size++] = '.';
        out[size++] = i < zeros ? '0' : digits[i - zeros];
    }
    return size;
}

// Returns the modifiers of the last pointer event that the round routed to `node`, or none.
PrAppModifierFlags pointerModifiers(const PrKit& kit, PrLayoutNode node) {
    PrUiEvents events = prUiEvents(kit.ui, node);
    PrAppModifierFlags modifiers = 0;
    for (uint32_t i = 0; i < events.count; ++i) {
        const PrAppEvent& e = events.events[i];
        if (e.kind == PrAppEventKind_PointerMove || e.kind == PrAppEventKind_PointerUp)
            modifiers = e.modifiers;
    }
    return modifiers;
}

// Returns the px that a scrub moved in the round from `from`, with each event's motion scaled by that event's
// modifiers. Motion is measured along the node's own x axis, so that a scrub on a turned or scaled panel follows the
// pointer over the field. A position that does not reach the node's plane adds no motion.
double scrubbed(const PrKit& kit, PrLayoutNode node, PrVec2 from, PrVec2 to) {
    PrVec2 local{};
    bool known = prUiLocalPoint(kit.ui, node, from, &local);
    float at = local.x;
    double total = 0;
    auto moveTo = [&](PrVec2 position, double factor) {
        PrVec2 mapped{};
        if (!prUiLocalPoint(kit.ui, node, position, &mapped))
            return;
        if (known)
            total += double(mapped.x - at) * factor;
        at = mapped.x;
        known = true;
    };
    PrUiEvents events = prUiEvents(kit.ui, node);
    for (uint32_t i = 0; i < events.count; ++i) {
        const PrAppEvent& e = events.events[i];
        if (e.kind == PrAppEventKind_PointerMove || e.kind == PrAppEventKind_PointerUp)
            moveTo(e.pointer.position, (e.modifiers & PrAppModifierFlag_Shift) != 0 ? kFineFactor : 1);
    }
    // Motion that no event accounts for, such as the round's last position past its events, is scaled as the last event
    // was.
    moveTo(to, (pointerModifiers(kit, node) & PrAppModifierFlag_Shift) != 0 ? kFineFactor : 1);
    return total;
}

// ---------------------------------------------------------------------------------------------------------------------
// Numeric fields

// What a numeric field keeps between rounds: the unquantized value that a scrub moves, and the value at which the scrub
// began.
struct NumberState {
    double accum;
    double start;
    bool live;                   // true while a gesture is open
    bool scrubbing;
};

// Applies the round's scrub to `number`.
void scrub(const PrKit& kit, PrLayoutNode node, const PrKitNumber& number, const NumberBounds& bounds, NumberState& state) {
    PrUiDrag drag{};
    bool dragging = prUiDrag(kit.ui, node, 0, kScrubThreshold, &drag);
    if (dragging && drag.moved && (drag.active || drag.released)) {
        PrVec2 from = drag.position - drag.delta;
        if (!state.scrubbing) {
            state.scrubbing = true;
            state.accum = state.start = scalarLoad(number.value);
            // The scrub counts from the press, including the motion below the threshold.
            from = drag.origin;
            beginEdit(kit, node, &state.live);
        }
        state.accum =
            clampF64(state.accum + scrubbed(kit, node, from, drag.position) * bounds.step, bounds.min, bounds.max);
        scalarWrite(kit, node, number.value, state.accum);
    }
    if (dragging && drag.cancelled && state.scrubbing)
        scalarWrite(kit, node, number.value, state.start);
    if (!dragging || !drag.active) {
        state.scrubbing = false;
        commitEdit(kit, node, &state.live);
    }
}

// Applies the round's key presses to `number`, each as its own gesture.
void nudge(const PrKit& kit, PrLayoutNode node, const PrKitNumber& number, const NumberBounds& bounds) {
    if (prUiFocus(kit.ui) != node)
        return;
    static const PrAppKey kKeys[] = {PrAppKey_Up, PrAppKey_Down};
    static const PrAppModifierFlags kModifiers[] = {0, PrAppModifierFlag_Shift};
    for (PrAppKey key : kKeys) {
        for (PrAppModifierFlags modifiers : kModifiers) {
            if (!prUiKeyPressed(kit.ui, node, key, modifiers))
                continue;
            double steps = modifiers != 0 ? kFineKeySteps : kKeySteps;
            double delta = steps * bounds.step;
            if (isIntegral(number.value.kind) && delta < 1)
                delta = 1;
            double value = scalarLoad(number.value) + (key == PrAppKey_Up ? delta : -delta);
            bool live = 0;
            beginEdit(kit, node, &live);
            scalarWrite(kit, node, number.value, clampF64(value, bounds.min, bounds.max));
            commitEdit(kit, node, &live);
        }
    }
}

PrLayoutNode numberField(PrKit& kit, PrLayoutNode parent, const PrKitNumber& number, const PrKitNumberStyle& style) {
    PR_ASSERT(number.value.at != nullptr);
    bool inRow = isRow(kit, parent);
    bool disabled = false;
    PrUiFlags interest = editFlags(PrUiFlag_Pressable | PrUiFlag_Focusable, number.disabled);
    PrLayoutNode node = control(kit, parent, number.key, interest, &disabled);
    NumberState* state = static_cast<NumberState*>(prUiState(kit.ui, node, &kKitOwner, StateKey_Number,
        sizeof(NumberState)));
    NumberBounds bounds = boundsOf(number.range, number.value.kind);
    if (!disabled) {
        scrub(kit, node, number, bounds, *state);
        nudge(kit, node, number, bounds);
        prUiSetCursor(kit.ui, node, PrAppCursor_ResizeX);
    } else {
        state->scrubbing = false;
        commitEdit(kit, node, &state->live);
    }
    uint8_t look = PartState_Rest;
    if (state->scrubbing || prUiFocus(kit.ui) == node)
        look = PartState_Active;
    else if (!disabled && prUiIsHovered(kit.ui, node))
        look = PartState_Hover;
    uint8_t flags = uint8_t((disabled ? PartFlag_Disabled : 0) | (inRow ? PartFlag_InRow : 0));
    prLayoutTreeFrameSetStyle(kit.tree, node,
        partStyle(kit, PartRecipe{.part = Part_NumberFrame, .size = style.size, .state = look, .flags = flags}));
    if (number.axis.size != 0) {
        PrKitTone tone = style.axisTone != PrKitTone_Auto ? style.axisTone : PrKitTone_Muted;
        textPart(kit, node, 0, PartRecipe{.part = Part_NumberAxis, .tone = tone}, number.axis);
    }
    char text[64];
    uint32_t precision = kDefaultPrecision;
    if (isIntegral(number.value.kind))
        precision = 0;
    else if (number.precision != 0)
        precision = number.precision;
    uint32_t size = formatNumber(scalarLoad(number.value), precision, text);
    textPart(kit, node, 0, PartRecipe{.part = Part_NumberValue, .flags = uint8_t(disabled ? PartFlag_Disabled : 0)},
        PrStringView{text, size});
    if (number.unit.size != 0)
        textPart(kit, node, 0, PartRecipe{.part = Part_NumberUnit}, number.unit);
    return node;
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

// What a text field keeps between rounds: the line of its history that the field shows, or the history's count for the
// line being typed, and whether it had focus.
struct TextState {
    uint32_t recalled;
    bool focused;
    bool chose;                     // a suggestion was chosen, and focus comes back from the list without reopening it
};

// Replaces the whole text with `value`, which stays valid for the round, as one gesture on `node`, and puts the caret
// at its end.
void replaceText(PrKit& kit, PrLayoutNode node, PrTextEditBuffer* PR_NULLABLE buffer, PrStringView value) {
    kitEditOnce(kit, node, [&] {
        kitReportText(kit, node, value);
        if (buffer != nullptr) {
            PrTextEditChange change{.start = 0, .end = prTextEditBufferSize(buffer), .text = value};
            prTextEditBufferApply(buffer, &change, 1, PrTextEditOrigin_Local);
        }
    });
    uint32_t end = uint32_t(value.size);
    prUiTextEditSetSelection(kit.ui, node, PrUiTextSelection{end, end});
}

// Recalls the history's earlier line with Up and its later line with Down, while the field has focus. Past the latest
// line is the line being typed, which starts empty. Returns the text that the field shows in this round.
PrStringView recall(PrKit& kit, PrLayoutNode node, const PrKitTextField& field, TextState& state) {
    bool up = prUiKeyPressed(kit.ui, node, PrAppKey_Up, 0);
    bool down = prUiKeyPressed(kit.ui, node, PrAppKey_Down, 0);
    uint32_t count = field.historyCount;
    if (state.recalled > count)
        state.recalled = count;
    if (up && state.recalled > 0) {
        --state.recalled;
        replaceText(kit, node, field.buffer, field.history[state.recalled]);
        return field.history[state.recalled];
    }
    if (down && state.recalled < count) {
        ++state.recalled;
        PrStringView line = state.recalled < count ? field.history[state.recalled] : PrStringView{""};
        replaceText(kit, node, field.buffer, line);
        return line;
    }
    return field.buffer != nullptr ? prTextEditBufferText(field.buffer) : field.text;
}

// Creates the element that holds the text of an edit on `control`, runs the edit, and creates the placeholder after the
// text while the text is empty.
void editedText(PrKit& kit, PrLayoutNode control, PrStringView text, PrTextEditBuffer* PR_NULLABLE buffer,
        PrStringView placeholder, const PrKitFieldStyle& style, bool disabled, bool area, PrUiTextEditFlags flags,
        const PrKitTextAreaStyle* PR_NULLABLE styles = nullptr, uint32_t styleCount = 0) {
    const PrKitPalette& p = kit.theme.palette;
    uint8_t textFlags = uint8_t((disabled ? PartFlag_Disabled : 0) | (style.mono ? PartFlag_Mono : 0) |
        (area ? PartFlag_Wraps : 0));
    PrLayoutNode value = part(kit, control, 0,
        PartRecipe{.part = area ? Part_AreaValue : Part_TextValue, .size = style.size, .flags = textFlags});
    // A style is an inline box in its own role and tone. The records come from the styles of this round, as a recipe's
    // do.
    RawArray<PrLayoutStyleIndex> records;
    if (styleCount == 0)
        records.push(kitStyle(kit, KitFont{.role = PrKitTextRole_Body}, KitToneInk{.tone = PrKitTone_Auto}));
    for (uint32_t i = 0; i < styleCount; ++i) {
        KitToneInk ink{.tone = kitTextTone(kit, styles[i].role, styles[i].tone)};
        records.push(kitStyle(kit, KitFont{.role = styles[i].role}, ink));
    }
    PrUiTextEditDesc edit{
        .control = control,
        .parent = value,
        .value = text,
        .buffer = buffer,
        .styles = records.data,
        .styleCount = records.count,
        .flags = PrUiTextEditFlags(flags | (area ? PrUiTextEditFlag_Multiline : 0)),
        .selection = kitColor16(p.textSelection),
        .caret = kitColor16(p.text1),
    };
    prUiTextEdit(kit.ui, &edit);
    records.reset();
    PrStringView changed{};
    if (prUiTextEditChanged(kit.ui, control, &changed)) {
        kitReportText(kit, control, changed);
        text = changed;
    }
    if (text.size == 0 && placeholder.size != 0 && !prUiTextEditIsComposing(kit.ui, control))
        textPart(kit, value, 0, PartRecipe{.part = Part_TextPlaceholder, .size = style.size, .flags = textFlags},
            placeholder);
}

// Creates a text field's frame and content, and writes to `arrived` whether focus came to it in the round. Returns the
// frame.
PrLayoutNode textField(PrKit* kit, PrLayoutNode parent, const PrKitTextField& field, const PrKitFieldStyle& style,
        bool* arrived) {
    PrStringView text = field.buffer != nullptr ? prTextEditBufferText(field.buffer) : field.text;
    bool inRow = isRow(*kit, parent);
    bool disabled = false;
    PrUiFlags editing = editFlags(PrUiFlag_Pressable | PrUiFlag_Focusable, field.disabled);
    PrLayoutNode node = control(*kit, parent, field.key, editing, &disabled);
    TextState* state = static_cast<TextState*>(prUiState(kit->ui, node, &kKitOwner, StateKey_Text, sizeof(TextState)));
    bool focused = prUiFocus(kit->ui) == node;
    *arrived = focused && !state->focused;
    state->focused = focused;
    if (!disabled) {
        prUiSetCursor(kit->ui, node, PrAppCursor_Text);
        if (field.historyCount != 0 && field.history != nullptr)
            text = recall(*kit, node, field, *state);
    }
    if (!focused)
        state->recalled = field.historyCount;
    uint8_t look = PartState_Active;
    if (!focused)
        look = pointerState(*kit, node, disabled) != PartState_Rest ? PartState_Hover : PartState_Rest;
    uint8_t flags = uint8_t((disabled ? PartFlag_Disabled : 0) | (inRow ? PartFlag_InRow : 0) |
        (ringed(*kit, node) ? PartFlag_Ring : 0) | (style.mono ? PartFlag_Mono : 0) | (style.bare ? PartFlag_Bare : 0));
    prLayoutTreeFrameSetStyle(kit->tree, node,
        partStyle(*kit, PartRecipe{.part = Part_TextFrame, .size = style.size, .state = look, .flags = flags}));
    if (field.icon != PrKitIconKind_None)
        prKitIcon(kit, node, {.icon = field.icon},
            {.size = 12, .tone = PrKitTone(disabled ? PrKitTone_Faint : PrKitTone_Muted)});
    editedText(*kit, node, text, field.buffer, field.placeholder, style, disabled, false,
        field.selectOnFocus ? PrUiTextEditFlag_SelectOnFocus : 0);
    if (field.clearable && text.size != 0 && !disabled) {
        PrLayoutNode clear = prLayoutTreeFrameElement(kit->tree, node, 0, 0);
        prUiSetFlags(kit->ui, clear, PrUiFlag_Pressable);
        prUiSetCursor(kit->ui, clear, PrAppCursor_Hand);
        prLayoutTreeFrameSetStyle(kit->tree, clear,
            partStyle(*kit, PartRecipe{.part = Part_TextClear, .state = pointerState(*kit, clear, false)}));
        prKitIcon(kit, clear, {.icon = PrKitIconKind_Close}, {.size = 10, .tone = PrKitTone_Muted});
        if (prUiActivated(kit->ui, clear)) {
            replaceText(*kit, node, field.buffer, "");
            prUiSetFocus(kit->ui, node);
        }
    }
    return node;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sliders

// What a slider keeps between rounds.
struct SliderState {
    bool live;                   // true while a gesture is open
};

double sliderValueAt(double low, double high, bool ratio, double t) {
    return ratio ? low * prF64Pow(high / low, t) : low + t * (high - low);
}

double sliderFractionAt(double low, double high, bool ratio, double value) {
    if (ratio) {
        double held = value > low ? value : low;
        return clampF64(prF64Log(held / low) / prF64Log(high / low), 0, 1);
    }
    double span = high - low;
    return span != 0 ? clampF64((value - low) / span, 0, 1) : 0;
}

// Applies the round's key presses to a slider at `fraction` of its track, each as its own gesture.
void sliderKeys(const PrKit& kit, PrLayoutNode node, const PrKitSlider& slider, double low, double high, bool ratio,
        double fraction) {
    if (prUiFocus(kit.ui) != node)
        return;
    double step = slider.step > 0 ? double(slider.step) : 0.01;
    struct Press {
        PrAppKey key;
        PrAppModifierFlags modifiers;
        double to;
    };
    const Press presses[] = {
        {PrAppKey_Left, 0, fraction - step},
        {PrAppKey_Right, 0, fraction + step},
        {PrAppKey_Left, PrAppModifierFlag_Shift, fraction - step * 10},
        {PrAppKey_Right, PrAppModifierFlag_Shift, fraction + step * 10},
        {PrAppKey_Home, 0, 0},
        {PrAppKey_End, 0, 1},
    };
    for (const Press& press : presses) {
        if (!prUiKeyPressed(kit.ui, node, press.key, press.modifiers))
            continue;
        bool live = 0;
        beginEdit(kit, node, &live);
        scalarWrite(kit, node, slider.value, sliderValueAt(low, high, ratio, clampF64(press.to, 0, 1)));
        commitEdit(kit, node, &live);
    }
}

PrLayoutNode sliderControl(PrKit& kit, PrLayoutNode parent, const PrKitSlider& content, const PrKitSliderStyle& style) {
    PR_ASSERT(content.value.at != nullptr);
    const PrKitPalette& p = kit.theme.palette;
    bool ratio = content.scale == PrKitSliderScale_Ratio;
    double low = content.min;
    double high = content.min == 0 && content.max == 0 ? 1 : content.max;
    PR_ASSERT(!ratio || (low > 0 && high > low));
    bool inRow = isRow(kit, parent);
    bool disabled = false;
    PrLayoutNode node = control(kit, parent, content.key,
        editFlags(PrUiFlag_Pressable | PrUiFlag_Focusable, content.disabled), &disabled);
    PrLayoutNode track = part(kit, node, 0, PartRecipe{.part = Part_SliderTrack});
    SliderState* state = static_cast<SliderState*>(prUiState(kit.ui, node, &kKitOwner, StateKey_Slider,
        sizeof(SliderState)));
    PrUiDrag drag{};
    bool dragging = !disabled && prUiDrag(kit.ui, node, 0, 0, &drag);
    if (dragging && (drag.active || drag.released)) {
        beginEdit(kit, node, &state->live);
        // The pointer is measured along the track in the track's own plane, so that the knob follows it under a turn.
        float width = kitExtent(kit, track).x;
        PrVec2 at{};
        if (width > 0 && high != low && prUiLocalPoint(kit.ui, track, drag.position, &at)) {
            double t = clampF64(double(at.x / width), 0, 1);
            scalarWrite(kit, node, content.value, sliderValueAt(low, high, ratio, t));
        }
    }
    if (!dragging || !drag.active)
        commitEdit(kit, node, &state->live);
    if (!disabled) {
        sliderKeys(kit, node, content, low, high, ratio, sliderFractionAt(low, high, ratio, scalarLoad(content.value)));
        prUiSetCursor(kit.ui, node, PrAppCursor_Hand);
    }
    uint8_t flags = uint8_t((inRow ? PartFlag_InRow : 0) | (ringed(kit, node) ? PartFlag_Ring : 0));
    prLayoutTreeFrameSetStyle(kit.tree, node,
        partStyle(kit, PartRecipe{.part = Part_SliderFrame, .size = style.size, .flags = flags}));
    float percent = float(sliderFractionAt(low, high, ratio, scalarLoad(content.value)) * 100);
    KitColors runFill{.background = disabled ? p.bg4 : p.accent};
    prLayoutTreeFrameElement(kit.tree, track, 0, kitStyle(kit, SliderRunBox{.percent = percent}, runFill));
    KitColors knobFill{.background = disabled ? p.text4 : prUiIsPressed(kit.ui, node, 0) ? p.accent : p.text2};
    prLayoutTreeFrameElement(kit.tree, track, 0, kitStyle(kit, SliderKnobBox{.percent = percent}, knobFill));
    return node;
}

// ---------------------------------------------------------------------------------------------------------------------
// The inspector

// What a group keeps between rounds if its caller keeps no flag.
struct GroupState {
    bool known;                  // true once `open` has been set
    bool open;
};

PrLayoutNode assetAction(PrKit* kit, PrLayoutNode gutter, PrKitIconKind icon, PrKitTone tone) {
    bool disabled = false;
    PrLayoutNode cell = control(*kit, gutter, 0, PrUiFlag_Pressable, &disabled);
    prLayoutTreeFrameSetStyle(kit->tree, cell,
        partStyle(*kit, PartRecipe{.part = Part_AssetAction, .state = pointerState(*kit, cell, disabled)}));
    if (!disabled)
        prUiSetCursor(kit->ui, cell, PrAppCursor_Hand);
    prKitIcon(kit, cell, {.icon = icon}, {.size = 11, .tone = PrKitTone(disabled ? PrKitTone_Faint : tone)});
    return cell;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitNumberField(PrKit* kit, PrLayoutNode parent, PrKitNumber number, PrKitNumberStyle style) {
    return numberField(*kit, parent, number, style);
}

PrLayoutNode prKitVectorField(PrKit* kit, PrLayoutNode parent, PrKitVector vector, PrKitNumberStyle style) {
    PR_ASSERT(vector.count >= 2 && vector.count <= 4 && vector.value.at != nullptr);
    PrKit& k = *kit;
    uint8_t flags = isRow(k, parent) ? PartFlag_InRow : 0;
    PrLayoutNode row = part(k, parent, vector.key, PartRecipe{.part = Part_Vector, .flags = flags});
    if (vector.disabled)
        prUiSetFlags(k.ui, row, PrUiFlag_Disabled);
    static const char kTags[] = "XYZW";
    static const PrKitTone kTones[] = {PrKitTone_AxisX, PrKitTone_AxisY, PrKitTone_AxisZ, PrKitTone_AxisW};
    for (uint32_t i = 0; i < vector.count; ++i) {
        PrKitNumber number{.value = scalarAt(vector.value, i), .range = vector.range, .precision = vector.precision};
        if (!vector.untagged)
            number.axis = PrStringView{kTags + i, 1};
        style.axisTone = kTones[i];
        numberField(k, row, number, style);
    }
    return row;
}

PrLayoutNode prKitTextField(PrKit* kit, PrLayoutNode parent, PrKitTextField field, PrKitFieldStyle style) {
    bool arrived = false;
    return textField(kit, parent, field, style, &arrived);
}

PrLayoutNode prKitTextArea(PrKit* kit, PrLayoutNode parent, PrKitTextArea area, PrKitFieldStyle style) {
    PrKit& k = *kit;
    bool inRow = isRow(k, parent);
    bool disabled = false;
    PrUiFlags editing = editFlags(PrUiFlag_Pressable | PrUiFlag_Focusable, area.disabled);
    PrLayoutNode node = control(k, parent, area.key, editing, &disabled);
    if (!disabled)
        prUiSetCursor(k.ui, node, PrAppCursor_Text);
    bool focused = prUiFocus(k.ui) == node;
    uint8_t look = PartState_Active;
    if (!focused)
        look = pointerState(k, node, disabled) != PartState_Rest ? PartState_Hover : PartState_Rest;
    uint8_t flags = uint8_t((disabled ? PartFlag_Disabled : 0) | (inRow ? PartFlag_InRow : 0) |
        (ringed(k, node) ? PartFlag_Ring : 0) | (style.mono ? PartFlag_Mono : 0));
    // The area's rows ride in the recipe's tone, up to the tone's range.
    uint8_t rows = uint8_t(area.rows < 255 ? area.rows : 255);
    prLayoutTreeFrameSetStyle(k.tree, node, partStyle(k, PartRecipe{
        .part = Part_AreaFrame,
        .size = style.size,
        .state = look,
        .tone = rows,
        .flags = flags,
    }));
    PrStringView text = area.buffer != nullptr ? prTextEditBufferText(area.buffer) : area.text;
    editedText(k, node, text, area.buffer, area.placeholder, style, disabled, true, 0, area.styles, area.styleCount);
    prUiAddFlags(k.ui, node, PrUiFlag_Scrollable);
    return node;
}

PrLayoutNode prKitSuggestField(PrKit* kit, PrLayoutNode parent, PrKitSuggestField suggest, PrKitFieldStyle style) {
    bool arrived = false;
    PrLayoutNode node = textField(kit, parent, suggest.field, style, &arrived);
    PrUi* ui = kit->ui;
    // The list opens as focus comes to the field, and the popover's own dismissals and a choice close it.
    TextState* state = static_cast<TextState*>(prUiState(ui, node, &kKitOwner, StateKey_Text, sizeof(TextState)));
    if (arrived && suggest.count != 0 && !state->chose)
        prKitSetOpen(kit, node, true);
    if (arrived)
        state->chose = false;
    PrRect box = prLayoutRectToPx(prLayoutNodeBounds(kit->tree, node, PrLayoutCoordinates_Viewport));
    PrKitPopoverStyle placed{.offset = PrVec2{0, 2}, .minWidth = box.right - box.left};
    PrLayoutNode popover = suggest.count != 0 ? prKitPopover(kit, node, {}, placed) : kPrLayoutNodeNone;
    if (popover == kPrLayoutNodeNone)
        return node;
    PrLayoutNode menu = prKitMenu(kit, popover, {}, {.leavesFocus = true});
    for (uint32_t i = 0; i < suggest.count; ++i) {
        if (prUiActivated(ui, prKitMenuItem(kit, menu, {.text = suggest.suggestions[i]}, {}))) {
            replaceText(*kit, node, suggest.field.buffer, suggest.suggestions[i]);
            prUiSetFocus(ui, node);
            state->chose = true;
        }
    }
    return node;
}

bool prKitTextChanged(const PrKit* kit, PrLayoutNode field, PrStringView* text) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    const KitTextChange* change = kitContext<KitTextChange>(*kit, field, KitContext_TextChange, &provider);
    if (change == nullptr || provider != field)
        return false;
    *text = change->text;
    return true;
}

PrLayoutNode prKitSlider(PrKit* kit, PrLayoutNode parent, PrKitSlider slider, PrKitSliderStyle style) {
    return sliderControl(*kit, parent, slider, style);
}

PrLayoutNode prKitInspectorGroup(PrKit* kit, PrLayoutNode parent, PrKitInspectorGroup group) {
    PrLayoutNode frame = part(*kit, parent, group.key, PartRecipe{.part = Part_GroupFrame});
    bool disabled = false;
    PrLayoutNode header = control(*kit, frame, 0, PrUiFlag_Pressable | PrUiFlag_Focusable, &disabled);
    bool* open = group.open;
    GroupState* kept = nullptr;
    if (open == nullptr) {
        kept = static_cast<GroupState*>(prUiState(kit->ui, frame, &kKitOwner, StateKey_Group, sizeof(GroupState)));
        if (!kept->known)
            *kept = GroupState{.known = 1, .open = 1};
    }
    if (prUiActivated(kit->ui, header)) {
        if (open != nullptr)
            *open = !*open;
        else
            kept->open = kept->open ? 0 : 1;
    }
    bool shown = open != nullptr ? *open : kept->open;
    bool ring = ringed(*kit, header) ? PartFlag_Ring : 0;
    prLayoutTreeFrameSetStyle(kit->tree, header, partStyle(*kit, PartRecipe{
        .part = Part_GroupHeader,
        .state = pointerState(*kit, header, disabled),
        .flags = ring,
    }));
    if (!disabled)
        prUiSetCursor(kit->ui, header, PrAppCursor_Hand);
    prKitIcon(kit, header, {.icon = PrKitIconKind(shown ? PrKitIconKind_ChevronDown : PrKitIconKind_ChevronRight)},
        {.size = 10, .tone = PrKitTone_Muted});
    PrKitTextStyle name{.role = PrKitTextRole_Strong, .tone = PrKitTone_Secondary, .wrap = PrKitWrap_Clip};
    prKitLabel(kit, header, {.text = group.name}, name);
    prKitSpacer(kit, header, {});
    if (group.badge.size != 0)
        prKitBadge(kit, header, {.text = group.badge}, {.variant = group.badgeVariant});
    if (!shown)
        return kPrLayoutNodeNone;
    return part(*kit, frame, 0, PartRecipe{.part = Part_GroupBody});
}

PrLayoutNode prKitInspectorRow(PrKit* kit, PrLayoutNode parent, PrKitInspectorRow row, PrKitInspectorRowStyle style) {
    PrLayoutNode node = prLayoutTreeFrameElement(kit->tree, parent, row.key, 0);
    uint8_t state = prUiIsHovered(kit->ui, node) ? PartState_Hover : PartState_Rest;
    prLayoutTreeFrameSetStyle(kit->tree, node, partStyle(*kit, PartRecipe{.part = Part_RowFrame, .state = state}));
    PrLayoutNode cell = part(*kit, node, 0, PartRecipe{.part = Part_RowLabelCell});
    textPart(*kit, cell, 0, PartRecipe{.part = Part_RowLabel, .flags = uint8_t(style.mono ? PartFlag_Mono : 0)},
        row.label);
    return part(*kit, node, 0, PartRecipe{.part = Part_RowControls});
}

PrLayoutNode prKitAssetField(PrKit* kit, PrLayoutNode parent, PrKitAssetField field) {
    PrKit& k = *kit;
    bool inRow = isRow(k, parent);
    PrLayoutNode node = prLayoutTreeFrameElement(k.tree, parent, field.key, 0);
    if (field.disabled)
        prUiSetFlags(k.ui, node, PrUiFlag_Disabled);
    bool disabled = prUiIsDisabled(k.ui, node);
    // The field is a target for its own kind alone, so that a payload of another kind passes it by for a target around
    // it.
    const PrUiPayload* carried = prUiDragging(k.ui);
    bool over = false;
    if (!disabled && carried != nullptr && carried->kind == field.kind)
        over = prUiIsDropTarget(k.ui, node);
    const PrUiPayload* dropped = disabled ? nullptr : prUiDropped(k.ui, node);
    if (dropped != nullptr && dropped->kind == field.kind) {
        kitProvide<AssetDrop>(k, node, KitContext_AssetDrop).value = dropped->value;
    }
    uint8_t flags = uint8_t((inRow ? PartFlag_InRow : 0) | (field.missing ? PartFlag_Missing : 0));
    prLayoutTreeFrameSetStyle(k.tree, node, partStyle(k, PartRecipe{
        .part = Part_AssetFrame,
        .state = over ? PartState_Active : PartState_Rest,
        .flags = flags,
    }));
    bool tinted = field.tint.a > 0;
    PartRecipe thumbPart{.part = Part_AssetThumb};
    PrLayoutStyleIndex thumbStyle;
    if (tinted)
        thumbStyle = kitStyle(k, InputBox{thumbPart}, InputFlex{thumbPart}, TintFill{.tint = field.tint});
    else
        thumbStyle = partStyle(k, thumbPart);
    PrLayoutNode thumb = prLayoutTreeFrameElement(k.tree, node, 0, thumbStyle);
    if (!tinted && field.icon != PrKitIconKind_None)
        prKitIcon(kit, thumb, {.icon = field.icon}, {.size = 11, .tone = PrKitTone_Muted});
    PrKitTone tone = field.missing ? PrKitTone_Danger : field.empty || disabled ? PrKitTone_Faint : PrKitTone_Primary;
    textPart(k, node, 0, PartRecipe{.part = Part_AssetName, .tone = tone}, field.name);
    PrLayoutNode gutter = part(k, node, 0, PartRecipe{.part = Part_AssetGutter});
    if (prKitActivated(kit, assetAction(kit, gutter, PrKitIconKind_Folder, PrKitTone_Muted)))
        kitSignal(k, node, KitSignal_Browsed);
    if (!field.empty) {
        if (prKitActivated(kit, assetAction(kit, gutter, PrKitIconKind_Target, PrKitTone_Muted)))
            kitSignal(k, node, KitSignal_Located);
        if (prKitActivated(kit, assetAction(kit, gutter, PrKitIconKind_Close, PrKitTone_Danger)))
            kitSignal(k, node, KitSignal_Cleared);
    }
    return node;
}

bool prKitBrowsed(const PrKit* kit, PrLayoutNode field) {
    return kitSignaled(*kit, field, KitSignal_Browsed);
}

bool prKitLocated(const PrKit* kit, PrLayoutNode field) {
    return kitSignaled(*kit, field, KitSignal_Located);
}

bool prKitCleared(const PrKit* kit, PrLayoutNode field) {
    return kitSignaled(*kit, field, KitSignal_Cleared);
}

bool prKitDropped(const PrKit* kit, PrLayoutNode field, uint64_t* value) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    const AssetDrop* drop = kitContext<AssetDrop>(*kit, field, KitContext_AssetDrop, &provider);
    if (drop == nullptr || provider != field)
        return false;
    *value = drop->value;
    return true;
}
