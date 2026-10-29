// The style of a node in the flex benchmark's trees. It holds the subset of CSS that every measured engine lays out, as
// plain data that each engine's adapter maps onto the engine's own types. Its memory layout follows C rules, so the
// Taffy adapter's Rust code (taffy/src/lib.rs) reads it directly. The two must change together.

#pragma once

#include "pr/base.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

// A size: auto, pixels, or a percentage, 0 to 100, of the containing block's size.
enum {
    FlexLengthKind_Auto,
    FlexLengthKind_Px,
    FlexLengthKind_Percent,
};
typedef uint8_t FlexLengthKind;

// A size of a kind, and its value in pixels or as a percentage.
struct FlexLength {
    FlexLengthKind kind;
    uint8_t _pad[3];
    float value;
};

// Whether a box is a flex container or is not laid out.
enum {
    FlexDisplay_Flex,
    FlexDisplay_None,
};
typedef uint8_t FlexDisplay;

// Static boxes are in flow. Relative boxes are in flow and are the containing block of their positioned descendants.
// Absolute boxes are out of flow and are placed by their insets within their parent. The parent counts as positioned
// whenever it has an absolute child.
enum {
    FlexPosition_Static,
    FlexPosition_Relative,
    FlexPosition_Absolute,
};
typedef uint8_t FlexPosition;

// Whether a box's overflow is visible or scrolls. A scroll container's automatic minimum size is zero.
enum {
    FlexOverflow_Visible,
    FlexOverflow_Scroll,
};
typedef uint8_t FlexOverflow;

// The flex container's main axis, and the direction along it.
enum {
    FlexDirection_Row,
    FlexDirection_Column,
    FlexDirection_RowReverse,
    FlexDirection_ColumnReverse,
};
typedef uint8_t FlexDirection;

// Whether a flex container's items wrap onto more lines, and in which direction.
enum {
    FlexWrap_NoWrap,
    FlexWrap_Wrap,
    FlexWrap_WrapReverse,
};
typedef uint8_t FlexWrap;

// The values of align-items, align-self, align-content and justify-content. Each property takes the values that apply
// to it. Normal is each property's initial value: stretch for align-items and align-content, the value of align-items
// for align-self, and flex-start for justify-content.
enum {
    FlexAlign_Normal,
    FlexAlign_FlexStart,
    FlexAlign_FlexEnd,
    FlexAlign_Center,
    FlexAlign_Stretch,
    FlexAlign_SpaceBetween,
    FlexAlign_SpaceAround,
    FlexAlign_SpaceEvenly,
    FlexAlign_Baseline,             // first baseline, for align-items and align-self
};
typedef uint8_t FlexAlign;

// Sides are top, right, bottom and left, in pixels.
struct FlexStyle {
    FlexDisplay display;
    FlexDirection direction;
    FlexWrap wrap;
    FlexAlign alignItems;
    FlexAlign alignSelf;
    FlexAlign alignContent;
    FlexAlign justifyContent;
    uint8_t marginAuto;             // one bit per side whose margin is auto, starting with top
    float grow;
    float shrink;
    FlexLength basis;
    FlexLength width;
    FlexLength height;
    FlexLength minWidth;
    FlexLength minHeight;
    FlexLength maxWidth;
    FlexLength maxHeight;
    float margin[4];
    float padding[4];
    float border[4];
    float gap;
    float aspectRatio;              // preferred width divided by height, or 0 for none
    FlexPosition position;
    FlexOverflow overflow;
    uint8_t _pad[2];
    FlexLength inset[4];            // a positioned box's insets, by side

    // Features that only lib/layout lays out, set only by the trees local to that engine: an item's order, and a leaf's
    // content of words. Words are inline-blocks laid out in lines, 20px tall and from 10 to 40px wide. If `percentWord`
    // is set, the first word's height is a percentage of the leaf's height.
    int32_t order;
    uint16_t words;
    bool percentWord;
    uint8_t _pad2;
};

// Returns CSS's initial values: a flex container in a row that does not wrap, with a grow factor of 0, a shrink factor
// of 1, an auto basis and auto sizes, and no margins, padding or borders.
inline FlexStyle flexStyleDefault() {
    FlexStyle style = {.display = FlexDisplay_Flex, .shrink = 1};
    return style;
}

PR_ASSUME_NONNULL_END
