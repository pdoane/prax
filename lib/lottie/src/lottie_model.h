// A Lottie animation as read: compositions, layers, shape items and animated properties.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#pragma once

#include "pr/array.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/lottie.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An index value that references nothing.
constexpr uint32_t kLottieNone = 0xffffffffu;

// The evaluator of an animation's expressions (lottie_expression.h).
struct LottieExpressions;

// A value of `dims` floats, fixed or keyframed. Values are runs of floats in the animation's `numbers`. A Bézier
// shape's floats are its closedness, its vertex count, then each vertex's point, in tangent and out tangent. The
// tangents are stored as points rather than offsets (the lottieShape functions).
struct LottieProperty {
    uint32_t value;             // fixed: the offset of the floats in `numbers`
    uint32_t keyframe;          // keyframed: the first keyframe
    uint32_t keyframeCount;     // zero if the value is fixed
    uint32_t dims;
    uint32_t expression;        // the offset of the source in `strings`; kLottieNone for none
    uint32_t expressionSize;
    uint32_t node;              // the property's node in the expression tree; kLottieNone for none
};

// A keyframe, which runs from `time` to the next keyframe's time, easing from `start` to `end`.
struct LottieKeyframe {
    double time;
    bool hold;                  // keeps the start value until the next keyframe
    uint32_t start;             // offsets in `numbers`
    uint32_t end;

    // Out x, out y, in x and in y for each of `easingDims`, in `numbers`. kLottieNone for linear.
    uint32_t easing;

    uint32_t easingDims;        // 1, or the property's dims if each dimension eases separately

    // The path that a position follows between the two values: `to` then `ti`, `dims` floats each, in `numbers`.
    // kLottieNone for a straight line.
    uint32_t spatial;
};

// A transform's properties. A missing property is kLottieNone, and its value is the identity.
struct LottieTransform {
    uint32_t anchor;
    uint32_t position;
    uint32_t positionX;         // set if the position is split into separate axes
    uint32_t positionY;
    uint32_t positionZ;
    uint32_t scale;             // percent
    uint32_t rotation;          // degrees clockwise
    uint32_t rotationX;         // 3D layers only
    uint32_t rotationY;
    uint32_t orientation;
    uint32_t skew;              // degrees
    uint32_t skewAxis;
    uint32_t opacity;           // percent
    uint32_t startOpacity;      // Repeater: percent
    uint32_t endOpacity;
};

// The kind of a shape layer's content item.
enum {
    LottieItemKind_Group,
    LottieItemKind_Path,
    LottieItemKind_Rect,
    LottieItemKind_Ellipse,
    LottieItemKind_Star,
    LottieItemKind_Fill,
    LottieItemKind_Stroke,
    LottieItemKind_GradientFill,
    LottieItemKind_GradientStroke,
    LottieItemKind_NoStyle,
    LottieItemKind_Transform,
    LottieItemKind_Trim,
    LottieItemKind_RoundCorners,
    LottieItemKind_Repeater,
    LottieItemKind_PuckerBloat,
    LottieItemKind_ZigZag,
    LottieItemKind_Twist,
    LottieItemKind_OffsetPath,
    LottieItemKind_Merge,
    LottieItemKind_Unknown,
};
typedef uint8_t LottieItemKind;

// The slot in LottieItem's `props` of each property, by item kind. Slots of different kinds overlap.
enum {
    LottieProp_Shape = 0,           // Path
    LottieProp_Position = 0,        // Rect, Ellipse, Star
    LottieProp_Size = 1,            // Rect, Ellipse; ZigZag: amplitude
    LottieProp_Roundness = 2,       // Rect; RoundCorners: radius
    LottieProp_OuterRadius = 1,     // Star
    LottieProp_OuterRoundness = 2,
    LottieProp_InnerRadius = 3,
    LottieProp_InnerRoundness = 4,
    LottieProp_Points = 5,          // Star; ZigZag: ridges per segment
    LottieProp_Rotation = 6,        // Star
    LottieProp_Color = 0,           // Fill, Stroke
    LottieProp_Opacity = 1,         // styles
    LottieProp_Width = 2,           // Stroke, GradientStroke
    LottieProp_Start = 3,           // gradients: points; Trim: percent
    LottieProp_End = 4,
    LottieProp_HighlightLength = 5, // radial gradients
    LottieProp_HighlightAngle = 6,
    LottieProp_Stops = 7,           // gradients
    LottieProp_Offset = 5,          // Trim: degrees; Repeater: copies; OffsetPath: amount
    LottieProp_Copies = 6,          // Repeater
    LottieProp_Amount = 0,          // PuckerBloat, OffsetPath, Twist
    LottieProp_Center = 1,          // Twist
    LottieProp_MiterLimit = 1,      // OffsetPath
    LottieProp_Count = 8,
};
typedef uint8_t LottieProp;

// A shape layer's content item. `kind` decides which fields it uses.
struct LottieItem {
    LottieItemKind kind;
    bool hidden;
    bool reversed;              // shapes: drawn counterclockwise
    PrPathFillRule rule;        // fills
    PrPathCap cap;              // strokes
    PrPathJoin join;            // strokes, OffsetPath
    PrDrawBlendMode blend;

    // Trim: 1 simultaneously, 2 individually. Repeater: 1 above, 2 below. Merge: the merge mode. Gradients: 1 linear, 2
    // radial. Star: 1 star, 2 polygon.
    uint8_t mode;

    float miterLimit;           // strokes
    uint32_t first;             // Group: its items; strokes: their dashes
    uint32_t count;
    uint32_t stops;             // gradients: the color stops, before any opacity stops
    uint32_t transform;         // Transform, Repeater: in `transforms`
    uint32_t props[LottieProp_Count];
};

// A stroke's dash, gap or offset.
enum {
    LottieDashKind_Dash,
    LottieDashKind_Gap,
    LottieDashKind_Offset,
};
typedef uint8_t LottieDashKind;

// A stroke's dash entry. `value` is its length property.
struct LottieDash {
    LottieDashKind kind;
    uint32_t value;
};

// How a mask combines with the masks before it.
enum {
    LottieMaskMode_None,
    LottieMaskMode_Add,
    LottieMaskMode_Subtract,
    LottieMaskMode_Intersect,
    LottieMaskMode_Lighten,
    LottieMaskMode_Darken,
    LottieMaskMode_Difference,
};
typedef uint8_t LottieMaskMode;

// A layer's mask. Its shape and amounts are properties.
struct LottieMask {
    LottieMaskMode mode;
    bool inverted;
    uint32_t shape;
    uint32_t opacity;           // percent
    uint32_t expansion;
    uint32_t feather;
};

// A layer's kind, as the animation numbers it.
enum {
    LottieLayerKind_Precomp = 0,
    LottieLayerKind_Solid = 1,
    LottieLayerKind_Image = 2,
    LottieLayerKind_Null = 3,
    LottieLayerKind_Shape = 4,
    LottieLayerKind_Text = 5,
    LottieLayerKind_Camera = 13,    // a view of its composition's 3D layers, drawing nothing itself
    LottieLayerKind_Other = 255,    // any kind the engine does not draw
};
typedef uint8_t LottieLayerKind;

// How a layer uses its matte layer.
enum {
    LottieMatte_None,
    LottieMatte_Alpha,
    LottieMatte_AlphaInverted,
    LottieMatte_Luma,
    LottieMatte_LumaInverted,
};
typedef uint8_t LottieMatte;

// A run of the animation's `strings`.
struct LottieString {
    uint32_t offset;
    uint32_t size;
};

// The kind of a node in the tree that expressions use: a composition, a layer, a group of nodes, or a property with a
// value.
enum {
    LottieNodeKind_Comp,
    LottieNodeKind_Layer,
    LottieNodeKind_Group,
    LottieNodeKind_Property,
};
typedef uint8_t LottieNodeKind;

// Which group or property a node is. Expressions find a node by its role, through accessors such as
// `transform.position` and through After Effects' names (lottie_expression.cpp).
enum {
    LottieRole_None,
    LottieRole_Transform,       // groups
    LottieRole_Contents,
    LottieRole_Effects,
    LottieRole_Masks,
    LottieRole_Effect,
    LottieRole_Mask,
    LottieRole_ShapeGroup,
    LottieRole_Shape,           // a path, rectangle, ellipse or star item
    LottieRole_Style,           // a fill, stroke or gradient item
    LottieRole_Modifier,
    LottieRole_AnchorPoint,     // properties
    LottieRole_Position,
    LottieRole_PositionX,
    LottieRole_PositionY,
    LottieRole_PositionZ,
    LottieRole_Scale,
    LottieRole_Rotation,
    LottieRole_RotationX,
    LottieRole_RotationY,
    LottieRole_Orientation,
    LottieRole_Opacity,
    LottieRole_Skew,
    LottieRole_SkewAxis,
    LottieRole_Path,
    LottieRole_Size,
    LottieRole_Roundness,
    LottieRole_Color,
    LottieRole_StrokeWidth,
    LottieRole_Start,
    LottieRole_End,
    LottieRole_Offset,
    LottieRole_Points,
    LottieRole_OuterRadius,
    LottieRole_InnerRadius,
    LottieRole_OuterRoundness,
    LottieRole_InnerRoundness,
    LottieRole_Copies,
    LottieRole_Amount,
    LottieRole_EffectValue,
    LottieRole_MaskPath,
    LottieRole_MaskOpacity,
    LottieRole_MaskExpansion,
    LottieRole_TimeRemap,
    LottieRole_Count,
};
typedef uint8_t LottieRole;

// A node in the tree that expressions use. Its children are a list in order.
struct LottieNode {
    LottieNodeKind kind;
    LottieRole role;
    LottieString name;
    LottieString matchName;
    int32_t index;              // `ix`: the index among the parent's properties; zero for none
    uint32_t parent;
    uint32_t firstChild;
    uint32_t lastChild;
    uint32_t nextSibling;
    uint32_t childCount;
    uint32_t property;          // Property: in `properties`
    uint32_t layer;             // Layer: in `layers`
    uint32_t comp;              // Comp, Layer: in `comps`
};

// A font that the animation's text names, and the host's fonts that its family and style match.
struct LottieFont {
    LottieString name;
    LottieString family;
    LottieString style;
    float ascent;               // percent of the size
};

// A glyph that the animation draws itself.
struct LottieCharShape {
    LottieString character;
    LottieString style;         // the font the glyph belongs to
    LottieString family;
    float width;                // the advance at a size of 100
    uint32_t first;             // the glyph's shape items
    uint32_t count;
};

// A letter of laid-out text: a grapheme cluster, placed in its line.
struct LottieLetter {
    float width;                // the advance
    float anchorWidth;          // the width of the group, which anchors the letter
    float add;                  // the total advance of the letters before this one in the group
    uint32_t line;
    uint32_t group;
    bool newline;
    bool space;
    uint32_t glyphs;            // in `textGlyphs`, relative to the letter's origin
    uint32_t glyphCount;

    // In `textTransforms`, one per glyph, about the glyph's origin. kLottieNone if every glyph is drawn at the
    // document's size.
    uint32_t transforms;

    uint32_t font;              // in `fonts`
    uint32_t shape;             // a glyph that the animation draws itself; kLottieNone for none
};

// A text layer's document at a keyframe, and its letters as laid out.
struct LottieTextDocument {
    double time;
    LottieString text;
    uint32_t font;              // in `textFonts`; kLottieNone for none
    float size;
    float tracking;             // thousandths of the size
    float lineHeight;
    float baselineShift;
    float strokeWidth;
    PrColor fill;
    PrColor stroke;
    bool hasFill;
    bool hasStroke;
    uint8_t justify;            // 0 left, 1 right, 2 center
    bool box;                   // paragraph text in a box, which wraps
    PrVec2 boxPosition;
    PrVec2 boxSize;
    uint32_t letters;
    uint32_t letterCount;
    uint32_t lines;             // the line widths, in `numbers`
    uint32_t lineCount;
    float boxWidth;
    float justifyOffset;
    float yOffset;              // between lines
    float ascent;

    // Each letter's index for each animator, in `letterIndexes`, with one run of letters per animator.
    uint32_t indexes;

    uint32_t totals;            // the number of units that each animator selects over, in `letterIndexes`
};

// A text animator: its range selector and the properties it animates. A missing property is kLottieNone.
struct LottieTextAnimator {
    uint32_t start;             // the selector's start, in its units
    uint32_t end;
    uint32_t offset;
    uint32_t easeHigh;
    uint32_t easeLow;
    uint32_t smoothness;
    uint32_t amount;
    uint8_t units;              // 1 percent, 2 index
    uint8_t basedOn;            // 1 characters, 2 characters excluding spaces, 3 words, 4 lines
    uint8_t shape;              // 1 square, 2 ramp up, 3 ramp down, 4 triangle, 5 round, 6 smooth
    uint32_t anchor;
    uint32_t position;
    uint32_t scale;
    uint32_t skew;
    uint32_t skewAxis;
    uint32_t rotation;
    uint32_t opacity;
    uint32_t strokeWidth;
    uint32_t strokeColor;
    uint32_t fillColor;
    uint32_t fillHue;
    uint32_t fillSaturation;
    uint32_t fillBrightness;
    uint32_t tracking;
};

// An effect's type, as the animation numbers After Effects' effects.
enum {
    LottieEffectType_Custom = 5,
    LottieEffectType_Tint = 20,
    LottieEffectType_Fill = 21,
    LottieEffectType_Stroke = 22,
    LottieEffectType_Tritone = 23,
    LottieEffectType_ProLevels = 24,
    LottieEffectType_DropShadow = 25,
    LottieEffectType_RadialWipe = 26,
    LottieEffectType_DisplacementMap = 27,
    LottieEffectType_Matte3 = 28,
    LottieEffectType_GaussianBlur = 29,
};
typedef uint8_t LottieEffectType;

// An effect value's type, as the animation numbers them.
enum {
    LottieEffectValueType_Slider = 0,
    LottieEffectValueType_Angle = 1,
    LottieEffectValueType_Color = 2,
    LottieEffectValueType_Point = 3,
    LottieEffectValueType_Checkbox = 4,
    LottieEffectValueType_Group = 5,    // holds values of its own
    LottieEffectValueType_None = 6,
    LottieEffectValueType_Dropdown = 7,
    LottieEffectValueType_Layer = 10,
};
typedef uint8_t LottieEffectValueType;

// A layer's effect. `values` and `count` are the run of its value properties in `effectValues`.
struct LottieEffect {
    LottieEffectType type;
    bool enabled;
    uint32_t values;
    uint32_t count;
};

// A layer of a composition. A field whose comment names layer kinds applies only to layers of those kinds.
struct LottieLayer {
    LottieLayerKind kind;
    int32_t id;                 // the index that the composition gives the layer
    uint32_t node;
    PrDrawBlendMode blend;
    LottieMatte matte;          // how the layer uses its matte layer
    bool matteSource;           // a matte for another layer, not drawn itself
    bool hidden;
    bool threeD;
    bool autoOrient;
    bool additive;              // blended by adding, which is not a blend mode
    uint32_t parent;            // in the composition's layers; kLottieNone for none
    uint32_t matteLayer;        // in the composition's layers; kLottieNone for none
    double inPoint;
    double outPoint;
    double startTime;
    double stretch;
    uint32_t transform;
    uint32_t first;             // Shape: its items; Text: its documents
    uint32_t count;
    uint32_t masks;
    uint32_t maskCount;
    uint32_t comp;              // Precomp: the composition it plays; Image: the image
    uint32_t timeRemap;         // Precomp: seconds; kLottieNone for none
    float width;                // Precomp, Solid
    float height;
    PrColor color;              // Solid
    uint32_t animators;         // Text
    uint32_t animatorCount;
    uint32_t alignment;         // Text: the anchor of each group
    uint8_t grouping;           // Text: 1 characters, 2 words, 3 lines, 4 all
    uint32_t effects;
    uint32_t effectCount;
    uint32_t zoom;              // Camera: the eye's distance from the image plane, in pixels
};

// A composition: a run of `layers`, and its size.
struct LottieComp {
    uint32_t layers;
    uint32_t count;
    float width;
    float height;
    uint32_t node;
};

// An image asset. An image layer covers the asset's size.
struct LottieImage {
    float width;
    float height;
    const PrDrawImage* PR_NULLABLE image;   // null if the image could not be read
};

// An animation's model. Everything lives in arrays that the animation owns, and records refer to each other by index.
// Animated values are runs of floats in `numbers`.
struct LottieAnimation {
    PrLottieInfo info;

    // The evaluator of the properties' expressions, created when the first one is evaluated (lottie_expression.h), and
    // the current depth of nested evaluation.
    mutable LottieExpressions* PR_NULLABLE expressions;
    mutable uint32_t expressionDepth;

    Array<LottieNode> nodes;            // the root first
    Array<LottieImage> images;
    Array<LottieComp> comps;            // the root first
    Array<LottieLayer> layers;
    Array<LottieItem> items;
    Array<LottieTransform> transforms;
    Array<LottieDash> dashes;
    Array<LottieMask> masks;
    Array<LottieProperty> properties;
    Array<LottieKeyframe> keyframes;
    Array<float> numbers;               // the pool of values
    Array<LottieEffect> effects;
    Array<uint32_t> effectValues;       // properties
    Array<char> strings;
    Array<LottieFont> textFonts;
    Array<const PrFont*> fonts;         // the animation holds a reference to each
    Array<LottieCharShape> charShapes;
    Array<LottieTextDocument> documents;
    Array<LottieTextAnimator> animators;
    Array<LottieLetter> letters;
    Array<uint32_t> letterIndexes;
    Array<PrFontGlyph> textGlyphs;
    Array<PrAffine2> textTransforms;
};

// Returns the text of `s`.
inline PrStringView lottieString(const LottieAnimation& a, LottieString s) {
    return PrStringView{s.size == 0 ? "" : a.strings.data + s.offset, s.size};
}

// Returns true if the Bézier shape `value` is closed.
inline bool lottieShapeClosed(const float* value) {
    return value[0] != 0;
}

// Returns the number of vertices in the Bézier shape `value`.
inline uint32_t lottieShapeCount(const float* value) {
    return uint32_t(value[1]);
}

// Returns the number of floats in a shape of `count` vertices.
inline uint32_t lottieShapeDims(uint32_t count) {
    return 2 + count * 6;
}

} // namespace pr_impl

// C opaque types
struct PrLottieAnimation : pr_impl::LottieAnimation {};

PR_ASSUME_NONNULL_END
