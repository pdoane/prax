// SVG styles: the user agent's sheet, and the computed values that the SVG tree reads from the cascade.
//
// SVG2 - https://svgwg.org/svg2-draft/styling.html
// SVG2 - https://svgwg.org/svg2-draft/painting.html

#pragma once

#include "pr/array.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/layout_animation.h"
#include "pr/path.h"
#include "pr/path_data.h"
#include "pr/path_stroke.h"
#include "pr/string_view.h"

#include "css/src/css_style.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The user agent's style sheet for SVG content (SVG2/styling.html#UAStyleSheet). xml:space="default" undoes an
// ancestor's preserve, as the attribute's nearest value decides (SVG2/text.html#LegacyXMLSpace), which the sheet that
// SVG 2 gives does not say. A foreignObject's overflow is hidden, as the table of
// SVG2/render.html#OverflowAndClipProperties gives it, though the sheet that SVG 2 gives leaves it visible.
constexpr char kSvgStyle[] = "@namespace url(http://www.w3.org/2000/svg);\n"
    "@namespace xml url(http://www.w3.org/XML/1998/namespace);\n"
    "svg:not(:root), image, marker, pattern, symbol, foreignObject { overflow: hidden; }\n"
    "*:not(svg), *:not(foreignObject) > svg { transform-origin: 0 0; }\n"
    "*[xml|space=preserve] { white-space-collapse: preserve-spaces; }\n"
    "*[xml|space=default] { white-space-collapse: collapse; }\n"
    ":link, :visited { cursor: pointer; }\n";

// The axis that a percentage is of: the width or the height of the nearest viewport, or its normalized diagonal
// (SVG2/coords.html#Units).
enum {
    SvgAxis_X,
    SvgAxis_Y,
    SvgAxis_Other,
};
typedef uint8_t SvgAxis;

// A length in user units whose percentage resolves when its viewport is known: `px` plus `percent` hundredths of the
// length that percentages are of. A calculation is held in this linear form, which it has unless it takes min() or
// max() of a percentage.
struct SvgLengthPercentage {
    float px;
    float percent;
};

// Returns the length in user units, with percentages of `base`.
inline float svgLengthResolve(SvgLengthPercentage length, float base) {
    return length.px + length.percent * base / 100;
}

// Returns the computed <length-percentage> of `property`. A keyword, such as auto, gives zero.
SvgLengthPercentage svgStyleLength(const CssComputedStyle* style, PrCssProperty property);

// The vector effects that change the user space of an element and its descendants (SVG2/coords.html#VectorEffects),
// which vector-effect lists. non-scaling-stroke changes only how the element strokes, and is not one of them.
enum {
    SvgVectorEffectFlag_NonScalingSize = 1u << 0,
    SvgVectorEffectFlag_NonRotation = 1u << 1,
    SvgVectorEffectFlag_FixedPosition = 1u << 2,
};
typedef uint8_t SvgVectorEffectFlags;

// Returns the vector effects that the computed vector-effect lists, besides non-scaling-stroke.
SvgVectorEffectFlags svgStyleVectorEffects(const CssComputedStyle* style);

// Returns true if the computed vector-effect lists non-scaling-stroke.
bool svgStyleNonScalingStroke(const CssComputedStyle* style);

// Returns true if the computed value of `property` is the keyword `keyword`.
bool svgStyleIs(const CssComputedStyle* style, PrCssProperty property, PrCssKeyword keyword);

// Returns the keyword that the computed value of `property` is, or PrCssKeyword_Count for any other value.
PrCssKeyword svgStyleKeyword(const CssComputedStyle* style, PrCssProperty property);

// Returns the computed number of `property`, or `fallback` if it is not a number.
float svgStyleNumber(const CssComputedStyle* style, PrCssProperty property, float fallback);

// Returns the computed color of `property` as extended sRGB, with currentcolor and the system colors resolved for the
// style in `environment`.
PrColor svgStyleColor(const CssComputedStyle* style, PrCssProperty property, const CssColorEnvironment& environment);

// What a fill or a stroke paints with (SVG2/painting.html#SpecifyingPaint).
enum {
    SvgPaintKind_None,
    SvgPaintKind_Color,
    SvgPaintKind_Url,           // a paint server, which the reference names
    SvgPaintKind_ContextFill,   // the fill of the context element (SVG2/painting.html#context-paint)
    SvgPaintKind_ContextStroke,
};
typedef uint8_t SvgPaintKind;

// A fill or a stroke as computed. A URL's server may be missing or unable to paint, and then its fallback paints: a
// color, or nothing if `fallback` is false.
struct SvgPaintStyle {
    SvgPaintKind kind;
    bool fallback;
    PrColor color;              // Color, or the fallback's color for Url
    PrStringView url;           // Url: the reference as computed, valid while the style lives
};

// Returns the computed paint of `property`, fill or stroke, with its colors resolved in `environment`.
SvgPaintStyle svgStylePaint(const CssComputedStyle* style, PrCssProperty property,
    const CssColorEnvironment& environment);

// The painting operations of a shape or text, which paint-order orders (SVG2/painting.html#PaintOrder).
enum {
    SvgLayer_Fill,
    SvgLayer_Stroke,
    SvgLayer_Markers,
};
typedef uint8_t SvgLayer;

// Writes the three layers to `order`, which holds three, in the order that the style's computed paint-order paints
// them.
void svgStylePaintOrder(const CssComputedStyle* style, SvgLayer* order);

// Returns the URL that the computed value of `property`, such as marker-start, references, or the null view for none.
// The view is valid while the style lives.
PrStringView svgStyleUrl(const CssComputedStyle* style, PrCssProperty property);

// Returns the URL that a computed value, such as an item of mask-image, starts with, or the null view if it does not
// start with a URL. The view is valid while the run is.
PrStringView svgRunUrl(CssValueRun run);

// Appends to `out` the commands of a computed d (SVG2/paths.html#TheDProperty) in absolute coordinates. none appends
// nothing. A fill rule in path() does not apply, because the fill-rule property decides how a path fills.
void svgStylePathCommands(const CssComputedStyle* style, Array<PrPathDataCommand>* out);

// The most dashes that a stroke holds. A longer list strokes without dashes.
constexpr uint32_t kSvgMaxDashes = 16;

// A stroke's shape as computed, with its lengths in user units until percentages resolve
// (SVG2/painting.html#StrokeProperties).
struct SvgStrokeStyle {
    SvgLengthPercentage width;
    SvgLengthPercentage dashOffset;
    SvgLengthPercentage dashes[kSvgMaxDashes];
    uint32_t dashCount;         // zero for none
    float miterLimit;
    PrPathJoin join;
    PrPathCap cap;
};

// Returns the shape of the stroke that the style computes.
SvgStrokeStyle svgStyleStroke(const CssComputedStyle* style);

// The lines of a text decoration (css-text-decor-3/#text-decoration-line-property).
enum {
    SvgLineFlag_Underline = 1u << 0,
    SvgLineFlag_Overline = 1u << 1,
    SvgLineFlag_LineThrough = 1u << 2,
};
typedef uint8_t SvgLineFlags;

// Returns the lines that the style's computed text-decoration-line draws.
SvgLineFlags svgStyleLines(const CssComputedStyle* style);

// Returns the computed fill-rule or clip-rule.
PrPathFillRule svgStyleFillRule(const CssComputedStyle* style, PrCssProperty property);

// When an element is the target of a pointer event (SVG2/interact.html#PointerEventsProp): over its fill, its stroke or
// both, if the element is visible, the fill or stroke paints, both, or neither, or over its bounding box. auto is
// VisiblePainted.
enum {
    SvgPointerEvents_None,
    SvgPointerEvents_BoundingBox,
    SvgPointerEvents_VisiblePainted,
    SvgPointerEvents_VisibleFill,
    SvgPointerEvents_VisibleStroke,
    SvgPointerEvents_Visible,
    SvgPointerEvents_Painted,
    SvgPointerEvents_Fill,
    SvgPointerEvents_Stroke,
    SvgPointerEvents_All,
};
typedef uint8_t SvgPointerEvents;

// Returns the computed pointer-events.
SvgPointerEvents svgStylePointerEvents(const CssComputedStyle* style);

// Returns true if an element whose pointer-events is `events` is hit over its stroke, and needs its stroke's shape
// whether or not it paints a stroke.
inline bool svgPointerReadsStroke(SvgPointerEvents events) {
    return events == SvgPointerEvents_VisibleStroke || events == SvgPointerEvents_Visible ||
        events == SvgPointerEvents_Stroke || events == SvgPointerEvents_All;
}

// The transform that a style gives an element (css-transforms-1/#transform-rendering): its transform functions, and
// their origin, whose percentages are of the reference box.
struct SvgTransformStyle {
    bool any;                   // false if the element has no transform, which leaves the rest unset
    PrLayoutTransformList list;
    SvgLengthPercentage origin[2]; // percentages of the reference box's width and height
    PrCssKeyword box;              // transform-box
};

// Returns the transform that the style gives the element.
SvgTransformStyle svgStyleTransform(const CssComputedStyle* style);

// Returns true if the style's transform property lists a 3D transform function
// (css-transforms-2/#3d-transformed-element), even one whose matrix is two-dimensional, such as translateZ(0).
bool svgStyleTransforms3d(const CssComputedStyle* style);

// Returns the matrix of `transform` for a reference box of `box`, with the origin as transform-origin places it.
PrAffine2 svgTransformMatrix(const SvgTransformStyle& transform, PrRect box);

// Writes the two offsets of a computed <position>, such as object-position, to `out`: x, then y. A percentage is of the
// space that the position places in along its axis, and an offset from the right or the bottom is measured back from
// its whole (css-values-4/#position).
void svgStylePosition(const CssComputedStyle* style, PrCssProperty property, SvgLengthPercentage* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
