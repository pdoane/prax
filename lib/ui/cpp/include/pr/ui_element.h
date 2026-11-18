// Elements in C++: nodes of the layout tree created in immediate mode, and styled by chained calls named for Tailwind
// CSS's utilities (pr/ui.h).
//
//     PrLayoutNode card = ui.div(root).flexCol().gap(8).p(16).rounded(12).bg(prColorFromHex(0x1e1b4b));
//     ui.span(card).textXl().bold().text("Hello");
//     if (ui.div(card).px(12).py(6).rounded(6).bg(blue).hoverBg(lighter).cursorPointer().text("Play").clicked())
//         restart();
//
// `div` creates its node at once, so a chain can ask about it as it goes: whether it was clicked, or is hovered. The
// calls of a chain write the element's style, which the builder turns into the node's record once the build creates its
// next element or returns. So an element's style is complete before its children are created, and its children inherit
// its text properties, as CSS's do. An element styled again after that opens again, at the cost of finding its record
// once more. Text properties that it sets then do not reach the children that it already has.
//
// The record is found through the context's memo (pr/ui.h, Records), so an element whose style is unchanged since the
// last frame costs one comparison of its style's bytes.
//
// A memo is a part of a build that a round keeps, unbuilt, while its inputs are unchanged (Memos, below).
//
// A builder is used from its PrUi's thread, during builds.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/bit_cast.h"
#include "pr/color.h"
#include "pr/construct.h"
#include "pr/font_set.h"
#include "pr/layout_frame.h"
#include "pr/layout_style.h"
#include "pr/layout_value.h"
#include "pr/path.h"
#include "pr/raw_array.h"
#include "pr/string_view.h"
#include "pr/type_traits.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

class UiBuilder;

// ---------------------------------------------------------------------------------------------------------------------
// Values

// A length along an axis. A number is px, so that `w(120)` reads as it means.
struct UiLength {
    PrLayoutLength value;

    PR_CONSTEXPR UiLength(float px) : value(prLayoutLengthPx(px)) {}
    PR_CONSTEXPR UiLength(int px) : value(prLayoutLengthPx(float(px))) {}
    explicit PR_CONSTEXPR UiLength(PrLayoutLength length) : value(length) {}
};

// Return a percentage of the containing block, all of it, and auto.
inline PR_CONSTEXPR UiLength uiPct(float percent) {
    return UiLength(prLayoutLengthPercent(percent));
}
inline PR_CONSTEXPR UiLength uiFull() {
    return UiLength(prLayoutLengthPercent(100));
}
inline PR_CONSTEXPR UiLength uiAuto() {
    return UiLength(prLayoutLengthAuto());
}

// The font families that builders set text in, by index: the sans and mono families of their fonts (UiResources), then
// those that UiResources::family registered.
enum : uint16_t {
    UiFamily_Sans,
    UiFamily_Mono,
};
typedef uint16_t UiFamily;

// ---------------------------------------------------------------------------------------------------------------------
// Style ops
//
// What an element's chained calls write. Code over elements reads none of this. It is in the header so that each call
// inlines to a store.

// The properties of a style, each an op's code. The header is the first op of a style, and holds what the record
// depends on beside the ops: the element's kind and number of ops in `aux` (kUiHeaderKindBits), and the parent's font
// and color groups in `a` and `b`, so that two styles compare by their headers first. The sides of padding, margins,
// insets and borders are bits of PrRectSide in the op's `aux`.
enum : uint8_t {
    UiOpCode_Header,
    UiOpCode_Display,
    UiOpCode_Position,
    UiOpCode_FlexDirection,
    UiOpCode_FlexWrap,
    UiOpCode_Grow,
    UiOpCode_Shrink,
    UiOpCode_Basis,
    UiOpCode_AlignItems,
    UiOpCode_AlignSelf,
    UiOpCode_AlignContent,
    UiOpCode_JustifyContent,
    UiOpCode_RowGap,
    UiOpCode_ColumnGap,
    UiOpCode_GridColumns,
    UiOpCode_GridRows,
    UiOpCode_ColumnSpan,
    UiOpCode_RowSpan,
    UiOpCode_Width,
    UiOpCode_Height,
    UiOpCode_MinWidth,
    UiOpCode_MinHeight,
    UiOpCode_MaxWidth,
    UiOpCode_MaxHeight,
    UiOpCode_AspectRatio,
    UiOpCode_Padding,
    UiOpCode_Margin,
    UiOpCode_Inset,
    UiOpCode_BorderWidth,
    UiOpCode_BorderColor,
    UiOpCode_BorderStyle,
    UiOpCode_Radius,
    UiOpCode_Background,
    UiOpCode_GradientKind,
    UiOpCode_GradientFrom,
    UiOpCode_GradientTo,
    UiOpCode_ShadowOffset,
    UiOpCode_ShadowBlur,
    UiOpCode_ShadowColor,
    UiOpCode_OutlineWidth,
    UiOpCode_OutlineColor,
    UiOpCode_OverflowX,
    UiOpCode_OverflowY,
    UiOpCode_Opacity,
    UiOpCode_ZIndex,
    UiOpCode_Blur,
    UiOpCode_BackdropBlur,
    UiOpCode_RotateX,
    UiOpCode_RotateY,
    UiOpCode_RotateZ,
    UiOpCode_TranslateX,
    UiOpCode_TranslateY,
    UiOpCode_TranslateZ,
    UiOpCode_Scale,
    UiOpCode_Perspective,
    UiOpCode_BackfaceHidden,
    UiOpCode_Visibility,
    UiOpCode_PointerEvents,
    UiOpCode_BoxSizing,
    UiOpCode_TextColor,
    UiOpCode_FontFamily,
    UiOpCode_FontSize,
    UiOpCode_FontWeight,
    UiOpCode_FontItalic,
    UiOpCode_LineHeight,
    UiOpCode_TextAlign,
    UiOpCode_TextWrap,
    UiOpCode_WhiteSpace,
    UiOpCode_LetterSpacing,
    UiOpCode_TextTransform,
    UiOpCode_Transition,
    UiOpCode_Count,
};
typedef uint8_t UiOpCode;

// One property: its code, a modifier such as the sides that it sets, and its value in up to 8 bytes.
PR_NO_PADDING_BEGIN
struct UiOp {
    UiOpCode code;
    uint8_t _pad;
    uint16_t aux;
    uint32_t a;
    uint32_t b;
};
PR_NO_PADDING_END

// What an element is: a block box, which its style may make a flex or grid container, or an inline box.
enum : uint8_t {
    UiKind_Div,
    UiKind_Span,
};
typedef uint8_t UiKind;

// The low bits of a header's `aux` that hold the element's kind. The bits above them hold the number of ops.
constexpr uint32_t kUiHeaderKindBits = 1;
constexpr uint32_t kUiMaxOps = 0xffffu >> kUiHeaderKindBits;

// The sides that an op of padding, margins, insets or borders sets: all, left and right, or top and bottom.
constexpr uint16_t kUiSidesAll = 0xf;
constexpr uint16_t kUiSidesX = (1u << PrRectSide_Left) | (1u << PrRectSide_Right);
constexpr uint16_t kUiSidesY = (1u << PrRectSide_Top) | (1u << PrRectSide_Bottom);

// Return an op that holds a number, two numbers, a length, an enumerated value or a color.
inline UiOp uiOpFloat(UiOpCode code, float value, uint16_t aux = 0) {
    return UiOp{.code = code, .aux = aux, .a = bitCast<uint32_t>(value)};
}
inline UiOp uiOpFloat2(UiOpCode code, float a, float b, uint16_t aux = 0) {
    return UiOp{.code = code, .aux = aux, .a = bitCast<uint32_t>(a), .b = bitCast<uint32_t>(b)};
}
inline UiOp uiOpLength(UiOpCode code, UiLength length, uint16_t aux = 0) {
    return UiOp{.code = code, .aux = aux, .a = length.value.bits};
}
inline UiOp uiOpValue(UiOpCode code, uint32_t value, uint16_t aux = 0) {
    return UiOp{.code = code, .aux = aux, .a = value};
}
inline UiOp uiOpColor(UiOpCode code, PrColor color, uint16_t aux = 0) {
    uint64_t packed = bitCast<uint64_t>(prColor16FromColor(color));
    return UiOp{.code = code, .aux = aux, .a = uint32_t(packed), .b = uint32_t(packed >> 32)};
}

// ---------------------------------------------------------------------------------------------------------------------
// Elements

// An element that the build just created, and whose style its chained calls write. Each call returns the element, so
// that calls chain, and an element converts to its node.
class UiElement {
public:
    UiElement(UiBuilder* builder, PrLayoutNode node) : builder_(builder), node_(node) {}

    // The element's node.
    PrLayoutNode node() const { return node_; }
    operator PrLayoutNode() const { return node_; }

    // ---- Display and position

    UiElement& block() { return set(uiOpValue(UiOpCode_Display, PrLayoutDisplay_Block)); }
    UiElement& inlineBlock() { return set(uiOpValue(UiOpCode_Display, PrLayoutDisplay_InlineBlock)); }
    UiElement& flex() { return set(uiOpValue(UiOpCode_Display, PrLayoutDisplay_Flex)); }
    UiElement& grid() { return set(uiOpValue(UiOpCode_Display, PrLayoutDisplay_Grid)); }
    UiElement& hidden() { return set(uiOpValue(UiOpCode_Display, PrLayoutDisplay_None)); }
    UiElement& relative() { return set(uiOpValue(UiOpCode_Position, PrLayoutPosition_Relative)); }
    UiElement& absolute() { return set(uiOpValue(UiOpCode_Position, PrLayoutPosition_Absolute)); }
    UiElement& fixed() { return set(uiOpValue(UiOpCode_Position, PrLayoutPosition_Fixed)); }
    UiElement& sticky() { return set(uiOpValue(UiOpCode_Position, PrLayoutPosition_Sticky)); }

    // ---- Flex

    // A flex container in a row or a column. Each makes the element a flex container.
    UiElement& flexRow() { return flex().set(uiOpValue(UiOpCode_FlexDirection, PrLayoutFlexDirection_Row)); }
    UiElement& flexCol() { return flex().set(uiOpValue(UiOpCode_FlexDirection, PrLayoutFlexDirection_Column)); }
    UiElement& flexWrap() { return set(uiOpValue(UiOpCode_FlexWrap, PrLayoutFlexWrap_Wrap)); }

    // How a flex item flexes: flex1 takes a share of the room left from no basis, and grow and shrink set each factor.
    UiElement& flex1() { return grow(1).shrink(1).basis(0); }
    UiElement& grow(float factor = 1) { return set(uiOpFloat(UiOpCode_Grow, factor)); }
    UiElement& shrink(float factor = 1) { return set(uiOpFloat(UiOpCode_Shrink, factor)); }
    UiElement& shrink0() { return shrink(0); }
    UiElement& basis(UiLength length) { return set(uiOpLength(UiOpCode_Basis, length)); }

    // Alignment of a container's items across its axis, of its content along it, and of the element itself.
    UiElement& itemsStart() { return items(PrLayoutAlignment_FlexStart); }
    UiElement& itemsEnd() { return items(PrLayoutAlignment_FlexEnd); }
    UiElement& itemsCenter() { return items(PrLayoutAlignment_Center); }
    UiElement& itemsBaseline() { return items(PrLayoutAlignment_Baseline); }
    UiElement& itemsStretch() { return items(PrLayoutAlignment_Stretch); }
    UiElement& justifyStart() { return justify(PrLayoutAlignment_FlexStart); }
    UiElement& justifyEnd() { return justify(PrLayoutAlignment_FlexEnd); }
    UiElement& justifyCenter() { return justify(PrLayoutAlignment_Center); }
    UiElement& justifyBetween() { return justify(PrLayoutAlignment_SpaceBetween); }
    UiElement& justifyAround() { return justify(PrLayoutAlignment_SpaceAround); }
    UiElement& justifyEvenly() { return justify(PrLayoutAlignment_SpaceEvenly); }
    UiElement& selfStart() { return alignSelf(PrLayoutAlignment_FlexStart); }
    UiElement& selfEnd() { return alignSelf(PrLayoutAlignment_FlexEnd); }
    UiElement& selfCenter() { return alignSelf(PrLayoutAlignment_Center); }
    UiElement& selfStretch() { return alignSelf(PrLayoutAlignment_Stretch); }
    UiElement& contentCenter() { return alignContent(PrLayoutAlignment_Center); }
    UiElement& items(PrLayoutAlignment a) { return set(uiOpValue(UiOpCode_AlignItems, a)); }
    UiElement& justify(PrLayoutAlignment a) { return set(uiOpValue(UiOpCode_JustifyContent, a)); }
    UiElement& alignSelf(PrLayoutAlignment a) { return set(uiOpValue(UiOpCode_AlignSelf, a)); }
    UiElement& alignContent(PrLayoutAlignment a) { return set(uiOpValue(UiOpCode_AlignContent, a)); }

    // The gaps between a container's items: both, between columns, or between rows.
    UiElement& gap(UiLength length) { return gapX(length).gapY(length); }
    UiElement& gapX(UiLength length) { return set(uiOpLength(UiOpCode_ColumnGap, length)); }
    UiElement& gapY(UiLength length) { return set(uiOpLength(UiOpCode_RowGap, length)); }

    // ---- Grid

    // A grid container of `count` equal columns or rows, each a share of the room that may shrink below its content, as
    // repeat(count, minmax(0, 1fr)). Each makes the element a grid container.
    UiElement& gridCols(uint32_t count) { return grid().set(uiOpValue(UiOpCode_GridColumns, count)); }
    UiElement& gridRows(uint32_t count) { return grid().set(uiOpValue(UiOpCode_GridRows, count)); }

    // How many of its grid's columns or rows a grid item spans.
    UiElement& colSpan(uint32_t count) { return set(uiOpValue(UiOpCode_ColumnSpan, count)); }
    UiElement& rowSpan(uint32_t count) { return set(uiOpValue(UiOpCode_RowSpan, count)); }

    // ---- Sizes

    UiElement& w(UiLength length) { return set(uiOpLength(UiOpCode_Width, length)); }
    UiElement& h(UiLength length) { return set(uiOpLength(UiOpCode_Height, length)); }
    UiElement& size(UiLength length) { return w(length).h(length); }
    UiElement& wFull() { return w(uiFull()); }
    UiElement& hFull() { return h(uiFull()); }
    UiElement& sizeFull() { return size(uiFull()); }
    UiElement& minW(UiLength length) { return set(uiOpLength(UiOpCode_MinWidth, length)); }
    UiElement& minH(UiLength length) { return set(uiOpLength(UiOpCode_MinHeight, length)); }
    UiElement& maxW(UiLength length) { return set(uiOpLength(UiOpCode_MaxWidth, length)); }
    UiElement& maxH(UiLength length) { return set(uiOpLength(UiOpCode_MaxHeight, length)); }
    UiElement& aspectRatio(float ratio) { return set(uiOpFloat(UiOpCode_AspectRatio, ratio)); }
    UiElement& aspectSquare() { return aspectRatio(1); }

    // An element's sizes include its padding and border, as Tailwind's preflight sets for every element, unless it
    // sizes its content box instead.
    UiElement& contentBox() { return set(uiOpValue(UiOpCode_BoxSizing, PrLayoutBoxSizing_ContentBox)); }

    // ---- Spacing

    // Padding, margins and a positioned box's insets: on every side, across, up and down, or on one side.
    UiElement& p(UiLength l) { return sides(UiOpCode_Padding, l, kUiSidesAll); }
    UiElement& px(UiLength l) { return sides(UiOpCode_Padding, l, kUiSidesX); }
    UiElement& py(UiLength l) { return sides(UiOpCode_Padding, l, kUiSidesY); }
    UiElement& pt(UiLength l) { return sides(UiOpCode_Padding, l, 1u << PrRectSide_Top); }
    UiElement& pr(UiLength l) { return sides(UiOpCode_Padding, l, 1u << PrRectSide_Right); }
    UiElement& pb(UiLength l) { return sides(UiOpCode_Padding, l, 1u << PrRectSide_Bottom); }
    UiElement& pl(UiLength l) { return sides(UiOpCode_Padding, l, 1u << PrRectSide_Left); }
    UiElement& m(UiLength l) { return sides(UiOpCode_Margin, l, kUiSidesAll); }
    UiElement& mx(UiLength l) { return sides(UiOpCode_Margin, l, kUiSidesX); }
    UiElement& my(UiLength l) { return sides(UiOpCode_Margin, l, kUiSidesY); }
    UiElement& mt(UiLength l) { return sides(UiOpCode_Margin, l, 1u << PrRectSide_Top); }
    UiElement& mr(UiLength l) { return sides(UiOpCode_Margin, l, 1u << PrRectSide_Right); }
    UiElement& mb(UiLength l) { return sides(UiOpCode_Margin, l, 1u << PrRectSide_Bottom); }
    UiElement& ml(UiLength l) { return sides(UiOpCode_Margin, l, 1u << PrRectSide_Left); }
    UiElement& mxAuto() { return mx(uiAuto()); }
    UiElement& inset(UiLength l) { return sides(UiOpCode_Inset, l, kUiSidesAll); }
    UiElement& top(UiLength l) { return sides(UiOpCode_Inset, l, 1u << PrRectSide_Top); }
    UiElement& right(UiLength l) { return sides(UiOpCode_Inset, l, 1u << PrRectSide_Right); }
    UiElement& bottom(UiLength l) { return sides(UiOpCode_Inset, l, 1u << PrRectSide_Bottom); }
    UiElement& left(UiLength l) { return sides(UiOpCode_Inset, l, 1u << PrRectSide_Left); }

    // ---- Borders and corners

    // Solid borders `width` px wide, on every side or on one side, in the color that borderColor sets, or in the
    // current text color without one.
    UiElement& border(float width = 1) { return borderOn(width, kUiSidesAll); }
    UiElement& borderT(float width = 1) { return borderOn(width, 1u << PrRectSide_Top); }
    UiElement& borderR(float width = 1) { return borderOn(width, 1u << PrRectSide_Right); }
    UiElement& borderB(float width = 1) { return borderOn(width, 1u << PrRectSide_Bottom); }
    UiElement& borderL(float width = 1) { return borderOn(width, 1u << PrRectSide_Left); }
    UiElement& borderColor(PrColor color) { return set(uiOpColor(UiOpCode_BorderColor, color, kUiSidesAll)); }
    UiElement& borderDashed() { return set(uiOpValue(UiOpCode_BorderStyle, PrDrawBorderStyle_Dashed)); }
    UiElement& borderDotted() { return set(uiOpValue(UiOpCode_BorderStyle, PrDrawBorderStyle_Dotted)); }

    // Corner radii in px, on every corner, or on the corners of one side.
    UiElement& rounded(float radius) { return set(uiOpFloat(UiOpCode_Radius, radius, 0xf)); }
    UiElement& roundedFull() { return rounded(9999); }
    UiElement& roundedT(float radius) { return corners(radius, PrRectCorner_TopLeft, PrRectCorner_TopRight); }
    UiElement& roundedB(float radius) { return corners(radius, PrRectCorner_BottomLeft, PrRectCorner_BottomRight); }
    UiElement& roundedL(float radius) { return corners(radius, PrRectCorner_TopLeft, PrRectCorner_BottomLeft); }
    UiElement& roundedR(float radius) { return corners(radius, PrRectCorner_TopRight, PrRectCorner_BottomRight); }

    // ---- Backgrounds and effects

    UiElement& bg(PrColor color) { return set(uiOpColor(UiOpCode_Background, color)); }

    // A gradient over the background color: linear along `degrees`, clockwise from up as CSS measures it, or radial
    // from the center out.
    UiElement& bgLinear(float degrees, PrColor from, PrColor to) {
        set(uiOpFloat(UiOpCode_GradientKind, degrees, PrLayoutGradientKind_Linear));
        set(uiOpColor(UiOpCode_GradientFrom, from));
        return set(uiOpColor(UiOpCode_GradientTo, to));
    }
    UiElement& bgRadial(PrColor inner, PrColor outer) {
        set(uiOpFloat(UiOpCode_GradientKind, 0, PrLayoutGradientKind_Radial));
        set(uiOpColor(UiOpCode_GradientFrom, inner));
        return set(uiOpColor(UiOpCode_GradientTo, outer));
    }

    // Shadows, the first on top: an outer shadow cast by the border box, or an inset one inside the padding box, offset
    // by `x` and `y` px, blurred by `blur` px and grown by `spread` px. The presets are Tailwind's scale of elevations,
    // and a ring is a shadow with no blur that the spread alone draws around the border box.
    UiElement& shadow(float x, float y, float blur, float spread, PrColor color, bool inset = false) {
        set(uiOpFloat2(UiOpCode_ShadowOffset, x, y, inset ? 1 : 0));
        set(uiOpFloat2(UiOpCode_ShadowBlur, blur, spread));
        return set(uiOpColor(UiOpCode_ShadowColor, color));
    }
    UiElement& shadowSm() { return shadow(0, 1, 2, 0, PrColor{0, 0, 0, 0.05f}); }
    UiElement& shadowMd() { return shadow(0, 4, 6, -1, PrColor{0, 0, 0, 0.1f}).shadow(0, 2, 4, -2, PrColor{0, 0, 0, 0.1f}); }
    UiElement& shadowLg() {
        return shadow(0, 10, 15, -3, PrColor{0, 0, 0, 0.1f}).shadow(0, 4, 6, -4, PrColor{0, 0, 0, 0.1f});
    }
    UiElement& shadowXl() {
        return shadow(0, 20, 25, -5, PrColor{0, 0, 0, 0.1f}).shadow(0, 8, 10, -6, PrColor{0, 0, 0, 0.1f});
    }
    UiElement& shadow2xl() { return shadow(0, 25, 50, -12, PrColor{0, 0, 0, 0.25f}); }
    UiElement& shadowInner() { return shadow(0, 2, 4, 0, PrColor{0, 0, 0, 0.05f}, true); }
    UiElement& ring(float width, PrColor color) { return shadow(0, 0, 0, width, color); }

    // An outline `width` px wide, `offset` px outside the border, which takes no room.
    UiElement& outline(float width, PrColor color, float offset = 0) {
        set(uiOpFloat2(UiOpCode_OutlineWidth, width, offset));
        return set(uiOpColor(UiOpCode_OutlineColor, color));
    }

    UiElement& opacity(float value) { return set(uiOpFloat(UiOpCode_Opacity, value)); }
    UiElement& z(int32_t level) { return set(uiOpValue(UiOpCode_ZIndex, uint32_t(level))); }

    // Blurs the element and what it holds, or what shows behind it, as frosted glass, by a standard deviation of `px`.
    UiElement& blur(float px) { return set(uiOpFloat(UiOpCode_Blur, px)); }
    UiElement& backdropBlur(float px) { return set(uiOpFloat(UiOpCode_BackdropBlur, px)); }

    // Overflow: clipped, or a scroll container in one or both axes, which the user scrolls with the wheel.
    UiElement& overflowHidden() { return overflow(PrLayoutOverflow_Clip, PrLayoutOverflow_Clip); }
    UiElement& overflowScroll() { return overflow(PrLayoutOverflow_Scroll, PrLayoutOverflow_Scroll); }
    UiElement& overflowXScroll() { return overflow(PrLayoutOverflow_Scroll, PrLayoutOverflow_Hidden); }
    UiElement& overflowYScroll() { return overflow(PrLayoutOverflow_Hidden, PrLayoutOverflow_Scroll); }

    // An element that takes room but draws nothing, and an element that the pointer passes through.
    UiElement& invisible() { return set(uiOpValue(UiOpCode_Visibility, PrLayoutVisibility_Hidden)); }
    UiElement& pointerEventsNone() { return set(uiOpValue(UiOpCode_PointerEvents, PrLayoutPointerEvents_None)); }

    // ---- Transforms

    // The element's transform about its center, as CSS writes one: each call appends a step, which applies to the
    // element before the steps before it. Angles are in degrees. The element still takes the pointer where it is drawn.
    // `perspective` is the distance in px from the eye to the plane of the element's children, which projects their 3D
    // transforms, and a backface-hidden element draws nothing while it faces away.
    UiElement& rotate(float degrees) { return set(uiOpFloat(UiOpCode_RotateZ, degrees)); }
    UiElement& rotateX(float degrees) { return set(uiOpFloat(UiOpCode_RotateX, degrees)); }
    UiElement& rotateY(float degrees) { return set(uiOpFloat(UiOpCode_RotateY, degrees)); }
    UiElement& translateX(float px) { return set(uiOpFloat(UiOpCode_TranslateX, px)); }
    UiElement& translateY(float px) { return set(uiOpFloat(UiOpCode_TranslateY, px)); }
    UiElement& translateZ(float px) { return set(uiOpFloat(UiOpCode_TranslateZ, px)); }
    UiElement& scale(float factor) { return set(uiOpFloat(UiOpCode_Scale, factor)); }
    UiElement& perspective(float px) { return set(uiOpFloat(UiOpCode_Perspective, px)); }
    UiElement& backfaceHidden() { return set(uiOpValue(UiOpCode_BackfaceHidden, 1)); }

    // Moves the element's colors, opacity and transform to new values over `seconds` instead of at once, eased as
    // Tailwind's transitions are, wherever the call stands in the chain.
    UiElement& transition(float seconds = 0.15f) { return set(uiOpFloat(UiOpCode_Transition, seconds)); }

    // ---- Text, which the element's descendants inherit

    UiElement& textColor(PrColor color) { return set(uiOpColor(UiOpCode_TextColor, color)); }

    // A font size in px, and Tailwind's scale of sizes, each with its line height.
    UiElement& textSize(float px) { return set(uiOpFloat(UiOpCode_FontSize, px)); }
    UiElement& textXs() { return textSize(12).leading(16); }
    UiElement& textSm() { return textSize(14).leading(20); }
    UiElement& textBase() { return textSize(16).leading(24); }
    UiElement& textLg() { return textSize(18).leading(28); }
    UiElement& textXl() { return textSize(20).leading(28); }
    UiElement& text2xl() { return textSize(24).leading(32); }
    UiElement& text3xl() { return textSize(30).leading(36); }
    UiElement& text4xl() { return textSize(36).leading(40); }
    UiElement& text5xl() { return textSize(48).leading(48); }
    UiElement& text6xl() { return textSize(60).leading(60); }

    // A font weight, as CSS numbers it, and Tailwind's names for them.
    UiElement& fontWeight(float weight) { return set(uiOpFloat(UiOpCode_FontWeight, weight)); }
    UiElement& light() { return fontWeight(300); }
    UiElement& normal() { return fontWeight(400); }
    UiElement& medium() { return fontWeight(500); }
    UiElement& semibold() { return fontWeight(600); }
    UiElement& bold() { return fontWeight(700); }
    UiElement& extrabold() { return fontWeight(800); }
    UiElement& black() { return fontWeight(900); }
    UiElement& italic(bool on = true) { return set(uiOpValue(UiOpCode_FontItalic, on ? 1 : 0)); }
    UiElement& font(UiFamily family) { return set(uiOpValue(UiOpCode_FontFamily, family)); }
    UiElement& mono() { return font(UiFamily_Mono); }

    // The height of a line of text in px, and the space added after each character in px.
    UiElement& leading(float px) { return set(uiOpFloat(UiOpCode_LineHeight, px)); }
    UiElement& tracking(float px) { return set(uiOpFloat(UiOpCode_LetterSpacing, px)); }

    UiElement& textLeft() { return set(uiOpValue(UiOpCode_TextAlign, PrLayoutTextAlign_Start)); }
    UiElement& textCenter() { return set(uiOpValue(UiOpCode_TextAlign, PrLayoutTextAlign_Center)); }
    UiElement& textRight() { return set(uiOpValue(UiOpCode_TextAlign, PrLayoutTextAlign_End)); }
    UiElement& uppercase() { return set(uiOpValue(UiOpCode_TextTransform, PrTextTransform_Uppercase)); }
    UiElement& capitalize() { return set(uiOpValue(UiOpCode_TextTransform, PrTextTransform_Capitalize)); }
    UiElement& whitespaceNowrap() { return set(uiOpValue(UiOpCode_TextWrap, PrLayoutTextWrapMode_Nowrap)); }
    UiElement& whitespacePre() { return set(uiOpValue(UiOpCode_WhiteSpace, PrLayoutWhiteSpaceCollapse_Preserve)); }

    // Appends `text` to the element's content, after what it already holds, its children included.
    UiElement& text(PrStringView text);

    // ---- States

    // Applies the style that `fn(element)` writes while the element is in a state: under the pointer, pressed by the
    // primary button, focused with a visible ring as a key press shows it, or disabled; or while `condition` holds.
    template <typename Fn>
    UiElement& hover(Fn&& fn) {
        return when(prUiIsHovered(ui(), node_), fn);
    }
    template <typename Fn>
    UiElement& active(Fn&& fn) {
        return when(prUiIsPressed(ui(), node_, 0), fn);
    }
    template <typename Fn>
    UiElement& focusVisible(Fn&& fn) {
        return when(prUiIsFocusVisible(ui(), node_), fn);
    }
    template <typename Fn>
    UiElement& disabledStyle(Fn&& fn) {
        return when(prUiIsDisabled(ui(), node_), fn);
    }
    template <typename Fn>
    UiElement& when(bool condition, Fn&& fn) {
        if (condition)
            fn(*this);
        return *this;
    }

    // The properties that change most under the pointer or a press, as Tailwind's hover: and active: variants set them.
    UiElement& hoverBg(PrColor color) { return hovered() ? bg(color) : *this; }
    UiElement& hoverText(PrColor color) { return hovered() ? textColor(color) : *this; }
    UiElement& hoverBorder(PrColor color) { return hovered() ? borderColor(color) : *this; }
    UiElement& hoverOpacity(float value) { return hovered() ? opacity(value) : *this; }
    UiElement& hoverScale(float factor) { return hovered() ? scale(factor) : *this; }
    UiElement& activeBg(PrColor color) { return pressed() ? bg(color) : *this; }
    UiElement& activeScale(float factor) { return pressed() ? scale(factor) : *this; }

    // ---- Input

    // Input that the element takes. A disabled element and everything created under it take none.
    UiElement& pressable() { return addFlags(PrUiFlag_Pressable); }
    UiElement& focusable() { return addFlags(PrUiFlag_Focusable); }
    UiElement& disabled(bool on = true) { return on ? addFlags(PrUiFlag_Disabled) : *this; }
    UiElement& cursor(PrAppCursor cursor) {
        prUiSetCursor(ui(), node_, cursor);
        return *this;
    }
    UiElement& cursorPointer() { return cursor(PrAppCursor_Hand); }

    // Returns true if the element was clicked in the current round: a primary click, or Enter or Space while it has
    // focus (prUiActivated). The element takes presses and focus.
    bool clicked() {
        addFlags(PrUiFlag_Pressable | PrUiFlag_Focusable);
        return prUiActivated(ui(), node_);
    }

    // Returns true if the pointer is over the element or its descendants, or if the element holds the primary button's
    // press.
    bool hovered() const { return prUiIsHovered(ui(), node_); }
    bool pressed() const { return prUiIsPressed(ui(), node_, 0); }

private:
    PrUi* ui() const;
    UiElement& set(UiOp op);
    UiElement& sides(UiOpCode code, UiLength l, uint16_t which) { return set(uiOpLength(code, l, which)); }
    UiElement& borderOn(float width, uint16_t which) { return set(uiOpFloat(UiOpCode_BorderWidth, width, which)); }
    UiElement& corners(float radius, PrRectCorner a, PrRectCorner b) {
        return set(uiOpFloat(UiOpCode_Radius, radius, uint16_t((1u << a) | (1u << b))));
    }
    UiElement& overflow(PrLayoutOverflow x, PrLayoutOverflow y);
    UiElement& addFlags(PrUiFlags flags) {
        prUiAddFlags(ui(), node_, flags);
        return *this;
    }

    UiBuilder* builder_;
    PrLayoutNode node_;
};

// ---------------------------------------------------------------------------------------------------------------------
// Animations

// The easing curves of Tailwind's transitions and animations: linear, ease-in, ease-out and ease-in-out.
enum : uint8_t {
    UiEase_Linear,
    UiEase_In,
    UiEase_Out,
    UiEase_InOut,
};
typedef uint8_t UiEase;

// When an animation runs, in seconds on the clock of the round's time (UiBuilder::time), and how its iterations play:
// each from its start to its end, or, if they alternate, every other one back.
struct UiAnimation {
    double start;
    double delay = 0;
    double duration = 0.3;
    double iterations = 1;          // may be infinite
    bool alternate = false;
    UiEase ease = UiEase_InOut;
};

// ---------------------------------------------------------------------------------------------------------------------
// Memos
//
// A view is functions that walk the application's model in every round, as immediate mode does, over the part of it
// that the view shows, and create elements of what they read. A function memoizes the part of its build that is worth
// it: the part's inputs, as a struct that compares with ==, and a build of them, which creates what they show on a root
// and under it. UiBuilder::memo creates the root and builds it, or keeps it as the last round built it. The struct is
// often local to the function, which keeps it beside the code that it memoizes, and keeps its build to its inputs:
//
//     UiElement badge(UiBuilder& ui, PrLayoutNode parent, PrStringView text, PrColor color) {
//         struct Memo {
//             String text;
//             PrColor color;
//             bool operator==(const Memo&) const = default;
//             void build(UiBuilder&, UiElement root) const { root.px(6).rounded(4).bg(color).text(text); }
//         };
//         return ui.memo(parent, Memo{.text = text, .color = color});
//     }
//
// A round keeps a memo's subtree, unbuilt, while its inputs equal the last round's, its root inherits the same text
// properties, and lib/ui finds that nothing the subtree owns has changed (prUiKeep): no input reached it, nothing of it
// moves on with time, and what its build asked of nodes outside it, and of the extents of its nodes, holds. A kept
// subtree costs a store for each of its nodes. Memos nest, and each decides on its own, so a change that reaches a memo
// inside a kept one builds that memo alone, again from the inputs that it kept.
//
// A memo saves building the elements of what its inputs show, and the walk that computed the inputs runs as before. Its
// build reads nothing but its inputs, because a kept memo's build does not run, so what the inputs hold is what decides
// when it builds again. A memo of a large part of the model, whose walk would cost too much in every round, holds what
// tells that part's changes, such as a generation that the model counts. Text that the inputs hold is a String that
// they own, or a view of static text, because the round compares the next inputs with its copy of these. The root's
// style places the memo, as the build gives it, and as the caller's calls on the root after memo give it.
//
// A build may return what happened to its part in the round, such as a click or an edit, which memo returns with the
// root (UiResult). A kept memo had nothing happen to it, and memo returns the result's default. A build that memoizes a
// part with a result depends on the part's root (prUiRead), so that input that reaches the part builds it too, and it
// reads the result.

// The type that the build of a memo of inputs `C` returns: what happened to its part in the round, or void.
template <typename C>
using UiMemoResult = decltype(declval<const C&>().build(declval<UiBuilder&>(), declval<UiElement>()));

// The root of a part that a build created, and what the part's build returned: a click or an edit, as a build that
// memoizes the part with UiBuilder::memo returns it.
template <typename R>
struct UiResult : R {
    UiElement root;

    operator PrLayoutNode() const { return root; }
};

// What UiBuilder::memo returns for a memo of inputs `C`: its root, or for a build that returns a result, its root and
// the result.
template <typename C>
using UiMemoReturn = Conditional<isSame<UiMemoResult<C>, void>, UiElement, UiResult<UiMemoResult<C>>>;

// The owner of the state of type `T` that builders keep on nodes (UiBuilder::state), whose address tells apart the
// states of different types on one node.
template <typename T>
inline constexpr char kUiStateOwner = 0;

// ---------------------------------------------------------------------------------------------------------------------
// Builders

// A path that an element draws: the path in the space of its view box, its color, and the width of its stroke in the
// view box's units, with its joins and caps, or zero to fill it by `rule` (PrLayoutPath).
struct UiPath {
    const PrPath* path;
    PrVec2 viewBox;
    PrColor color;
    float strokeWidth = 0;
    PrPathJoin join = PrPathJoin_Round;
    PrPathCap cap = PrPathCap_Round;
    PrPathFillRule rule = PrPathFillRule_NonZero;
};

// What builders draw with, which outlives them: the fonts that they set text in, the families of a font set by UiFamily
// and the fonts made of them in each size, weight and slant that an element's style asks for; and the paths that path
// data describes, each parsed once (prPathCacheFind). Builders share them, so that a builder made again for a new
// context, as a window's is when its device is lost, draws with the fonts and the paths that the last one did, whose
// glyphs and rasters are cached already. `set` must outlive them; with none, text is set in the tree's default font.
// Resources are used from one thread.
class UiResources {
public:
    explicit UiResources(PrFontSet* PR_NULLABLE set, PrFontFamily sans = prFontFamilyGeneric(PrFontGeneric_SystemUi),
        PrFontFamily mono = prFontFamilyGeneric(PrFontGeneric_Monospace));
    ~UiResources();
    UiResources(const UiResources&) = delete;
    UiResources& operator=(const UiResources&) = delete;

    // Registers a font family, and returns the UiFamily that names it.
    UiFamily family(PrFontFamily family);

    // Returns the path that `data`, SVG path data, describes, which the resources parse on its first use and keep, so
    // that the same data gives the same path in every round.
    const PrPath* path(PrStringView data);

    // The resources' state, which their source defines.
    struct State;

private:
    friend class UiBuilder;

    State* state_;
};

// The context that a builder creates elements on, what it draws with, and what it sets text in where nothing above an
// element sets it: the size, weight and color of its text. `resources` must outlive the builder. With no resources,
// text is set in the tree's default font.
struct UiBuilderDesc {
    PrUi* ui;
    UiResources* PR_NULLABLE resources = nullptr;
    float textSize = 16;
    float fontWeight = 400;
    PrColor textColor = {0, 0, 0, 1};
};

// Creates elements on one context, and the fonts of their text. A builder is created and destroyed between rounds, and
// outlives no context that it creates elements on. A context keeps the subtrees of a builder's memos while the builder
// lives.
class UiBuilder {
public:
    explicit UiBuilder(const UiBuilderDesc& desc);
    ~UiBuilder();
    UiBuilder(const UiBuilder&) = delete;
    UiBuilder& operator=(const UiBuilder&) = delete;

    // Creates a block element or an inline element under `parent`, keyed by `key` among the parent's children if it is
    // not zero.
    UiElement div(PrLayoutNode parent, PrUiKey key = 0);
    UiElement span(PrLayoutNode parent, PrUiKey key = 0);

    // Creates a replaced element under `parent`, keyed by `key` among the parent's children if it is not zero, a block
    // box whose content the caller sets (prLayoutTreeFrameSetContent).
    UiElement replaced(PrLayoutNode parent, PrUiKey key = 0);

    // Creates a replaced element under `parent` that draws `path` (PrLayoutPath), keyed by `key` among the parent's
    // children if it is not zero. Its natural size is the path's view box in px, and its style sizes it as any
    // element's does: `.size(14)` draws the path scaled to 14 px.
    UiElement path(PrLayoutNode parent, const UiPath& path, PrUiKey key = 0);

    // Creates the root of a memo of `inputs` under `parent`, keyed by `key` among the parent's children if it is not
    // zero, and keeps its subtree or builds it (Memos). Returns the root, or for a build that returns a result, the
    // root and the result in a UiResult.
    template <typename C>
    UiMemoReturn<C> memo(PrLayoutNode parent, const C& inputs, PrUiKey key = 0) {
        if constexpr (isSame<UiMemoResult<C>, void>) {
            return memoBuild(parent, key, MemoTypeOf<C>::kType, &inputs, nullptr);
        } else {
            UiMemoResult<C> result{};
            UiElement root = memoBuild(parent, key, MemoTypeOf<C>::kType, &inputs, &result);
            return UiMemoReturn<C>{result, root};
        }
    }

    // The context that the builder creates elements on, what it draws with, its tree, and the time of the round being
    // built, in seconds.
    PrUi* ui() const;
    UiResources* PR_NULLABLE resources() const;
    PrLayoutTree* tree() const;
    double time() const;

    // Returns the eased progress of `animation` at the round's time: 0 before it starts, rising to 1 over each
    // iteration, and where its last iteration left it once it ends. While it runs, or waits out its delay, the context
    // has a frame for it, and the memo being built builds again in that frame. animate returns the value at that
    // progress between `from` and `to`. A memo that reads time() builds again in every frame.
    float progress(const UiAnimation& animation);
    float animate(const UiAnimation& animation, float from, float to);

    // Returns the `T` kept on `node` under `key`, which starts zeroed and lasts while each round asks for it, as
    // prUiState keeps state, and as long as a round keeps the node. A memo keeps what its inputs do not hold this way,
    // such as where a drag began, on its root. No constructor or destructor runs, so `T` is trivial.
    template <typename T>
    T& state(PrLayoutNode node, uint64_t key = 0) {
        static_assert(isTriviallyCopyable<T> && isTriviallyDestructible<T>, "a node's state is plain data");
        return *static_cast<T*>(prUiState(ui(), node, &kUiStateOwner<T>, key, uint32_t(sizeof(T))));
    }

    // The builder's state, which its source defines.
    struct State;

private:
    friend class UiElement;

    // Returns the ops of `node`'s style, to which a chained call appends, opening the element again if another is open.
    RawArray<UiOp>& opsOf(PrLayoutNode node);

    // The inputs of a memo of the builder, as lib/ui's memo holds them (prUiMemo): the text groups that the root
    // inherits, which the builder's records hold, so that a change of them builds the memo again, and the C++ inputs,
    // which the copy that lib/ui keeps owns.
    struct MemoInputs {
        PrLayoutGroupIndex font;
        PrLayoutGroupIndex color;
        const void* inputs;
    };

    // Begins and ends the build of a memo on `root`, which lib/ui created, for the builder's state `context`: opens the
    // root for the build to style, and keeps the ops of its style for a later round that keeps it. memoBegin returns
    // the builder.
    static UiBuilder& memoBegin(void* PR_NULLABLE context, PrLayoutNode root);
    static void memoEnd(void* PR_NULLABLE context, PrLayoutNode root);

    // Returns true if two memos' roots inherit the same text groups.
    static bool memoInherits(const MemoInputs& a, const MemoInputs& b) { return a.font == b.font && a.color == b.color; }

    // The type of memo of inputs `C`, as lib/ui's memo holds it (PrUiMemoType), and the operations that it calls on
    // MemoInputs. A memo whose build returns nothing is given no result.
    //
    // The type is a static member of a class template, as pr::Function's operations are, not a member variable
    // template: MSVC initializes a member variable template of this type at run time (C4268).
    template <typename C>
    struct MemoTypeOf {
        static void build(void* PR_NULLABLE context, PrUi*, PrLayoutNode root, const void* inputs, void* PR_NULLABLE result) {
            UiBuilder& ui = memoBegin(context, root);
            const C& c = *static_cast<const C*>(static_cast<const MemoInputs*>(inputs)->inputs);
            if constexpr (isSame<UiMemoResult<C>, void>) {
                PR_ASSERT(result == nullptr);
                c.build(ui, UiElement(&ui, root));
            } else {
                UiMemoResult<C> out = c.build(ui, UiElement(&ui, root));
                if (result != nullptr)
                    *static_cast<UiMemoResult<C>*>(result) = out;
            }
            memoEnd(context, root);
        }
        static bool equal(const void* a, const void* b, uint32_t) {
            const MemoInputs& x = *static_cast<const MemoInputs*>(a);
            const MemoInputs& y = *static_cast<const MemoInputs*>(b);
            return memoInherits(x, y) && bool(*static_cast<const C*>(x.inputs) == *static_cast<const C*>(y.inputs));
        }
        static void copy(void* to, const void* from, uint32_t) {
            const MemoInputs& source = *static_cast<const MemoInputs*>(from);
            *static_cast<MemoInputs*>(to) = MemoInputs{
                .font = source.font,
                .color = source.color,
                .inputs = create<C>(*static_cast<const C*>(source.inputs)),
            };
        }
        static void release(void* inputs) {
            destroy(static_cast<C*>(const_cast<void*>(static_cast<MemoInputs*>(inputs)->inputs)));
        }
        static constexpr PrUiMemoType kType = {.build = build, .equal = equal, .copy = copy, .destroy = release};
    };

    // Creates a memo's root with lib/ui's memo (prUiMemo), and keeps its subtree or builds it from `inputs` of `type`,
    // writing its result, which starts as nothing having happened, to `result` if it has one.
    UiElement memoBuild(PrLayoutNode parent, PrUiKey key, const PrUiMemoType& type, const void* inputs,
        void* PR_NULLABLE result);

    // The builder's state, which the builder allocates and frees.
    State* state_;
};

// Appends `op` to the element's style.
inline UiElement& UiElement::set(UiOp op) {
    builder_->opsOf(node_).push(op);
    return *this;
}

} // namespace pr

PR_ASSUME_NONNULL_END
