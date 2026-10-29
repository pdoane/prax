// Lowering: draw list primitives broken into simpler parts, for a backend that cannot draw a primitive directly.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/path_rasterizer.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Shapes and strokes

// Appends the shape of `command` to `out`, as a path in the command's space. The command is a shape, a glyph run, or a
// clip push other than PushClipTargetRect. A rectangle with no area still has its four sides, which a stroke follows. A
// glyph run's shape is every glyph's outline.
void prDrawShapePath(const PrDrawCommand* command, PrPath* out);

// Returns the bounds of a painting command's fill geometry in the command's space, without the stroke. An ObjectBounds
// brush maps its unit square onto these bounds. A glyph run's bounds are the union of its glyphs' cells. A cell is as
// wide as the glyph's advance and spans from the font's ascent to its descent.
PrRect prDrawFillBounds(const PrDrawCommand* command);

// Returns how far the stroke's outline may lie from its shape, as a multiple of the stroke's width (prPathStrokeReach).
// A stroke aligned inside or outside reaches twice as far.
float prDrawStrokeReach(const PrDrawStroke* stroke);

// Lowers `path`, stroked as `stroke` under `transform`, into fills. Appends outlines in target pixels, which fill with
// the nonzero rule, to the three polylines.
//
// A centered stroke appends its outline to `fill`, and nothing to the others. A stroke aligned inside or outside
// appends three parts. `aligned` gets the closed contours stroked at twice the width. `region` gets the closed
// contours, mapped and flattened. `fill` gets the open contours, stroked centered. The stroke of the closed contours is
// `aligned` cut to the nonzero interior of `region` for an inside stroke, or to its exterior for an outside stroke.
//
// Curves and round parts flatten to within `tolerance` target pixels. Width, dashes and caps scale with the transform
// unless the stroke is non-scaling.
void prDrawLowerStroke(const PrPath* path, const PrDrawStroke* stroke, PrAffine2 transform, float tolerance,
                       PrPathPolyline* fill, PrPathPolyline* aligned, PrPathPolyline* region);

// ---------------------------------------------------------------------------------------------------------------------
// Brushes

// Writes the transform from target pixels to the brush's space to `out`, for a command drawn under `transform` whose
// fill geometry has `bounds`. Returns false if the mapping is singular, such as for an ObjectBounds brush over bounds
// with no area. Such a brush paints nothing.
bool prDrawBrushFromTarget(const PrDrawBrush* brush, PrAffine2 transform, PrRect bounds, PrAffine2* out);

// Writes the gradient's parameter at `point` of brush space, before it extends, to `t`. Returns false at points where a
// radial gradient paints nothing.
bool prDrawGradientParameter(const PrDrawBrush* brush, PrVec2 point, float* t);

// Maps `t` by extending the gradient with `extend`: into [0, 1] for Repeat and Reflect, and into [-1, 2] for Pad, which
// keeps a point before the start below 0 and one past the end above 1, where a ramp's end pieces extend its end colors.
// Returns false if the gradient paints nothing at `t`, which is the case past its range with None.
bool prDrawGradientExtend(PrDrawExtend extend, float* t);

// Returns a hash of the inputs that decide a gradient's ramp (prDrawGradientRamp). A backend uses it as the key for the
// ramps that it caches. Brushes that differ only in geometry, extend or space share a ramp.
uint64_t prDrawGradientRampHash(const PrDrawBrush* brush, PrColor color);

// Returns true if two gradients, each with the color it multiplies, have the same ramp, judged by the inputs that
// prDrawGradientRampHash hashes.
bool prDrawGradientRampEquals(const PrDrawBrush* a, PrColor aColor, const PrDrawBrush* b, PrColor bColor);

// A span of a gradient's parameter whose colors a backend interpolates between. Pieces meet where the gradient changes
// color, at its stops, and an interpolation never crosses from one piece into the next, so a stop that changes color
// abruptly stays exact at any resolution. The pieces of a ramp run in order from -1 to 2: the first stop's color before
// 0, the gradient's colors from 0 to 1, and the last stop's color past 1.
typedef struct PrDrawRampPiece {
    float start;                // the parameter where the piece starts
    float end;                  // the parameter where the piece ends
    uint32_t first;             // the piece's first color in the ramp's colors
    uint32_t count;             // colors at even steps from `start` to `end`, or one for a piece of one color
} PrDrawRampPiece;

// The maximum numbers of pieces, and of colors, in the ramp of a gradient with `stopCount` stops at `resolution`.
uint32_t prDrawGradientRampMaxPieces(uint32_t stopCount);
uint32_t prDrawGradientRampMaxColors(uint32_t stopCount, uint32_t resolution);

// Writes the gradient's ramp: its pieces to `pieces`, which holds prDrawGradientRampMaxPieces of them, and their colors
// to `colors`, which holds prDrawGradientRampMaxColors. Returns the number of pieces. A piece whose colors mix linearly
// in `space` has two colors, its ends. A piece that mixes in another space, by a hint, around a hue, or straight
// between stops that differ in both color and alpha has a color at every 1/`resolution` of the parameter, and at its
// ends. Each color is the brush's color times `color` (a command's, straight alpha), premultiplied, with channels in
// `space`, which is sRGB or sRGB-linear.
uint32_t prDrawGradientRamp(const PrDrawBrush* brush, PrColor color, PrColorSpace space, uint32_t resolution,
                            PrDrawRampPiece* pieces, PrColor* colors);

// The number of colors in a ramp of `count` pieces.
uint32_t prDrawRampColorCount(const PrDrawRampPiece* PR_NULLABLE pieces, uint32_t count);

// Returns the index of the piece that holds `t`, in [-1, 2], searching forward from `hint`, the index of a piece, such
// as zero or the piece that held the last point. A point where two pieces meet is in the later piece, except at 1,
// which is the end of the gradient's colors and not yet past them.
uint32_t prDrawRampPieceAt(const PrDrawRampPiece* pieces, float t, uint32_t hint);

// Returns the ramp's color at `t`, in [-1, 2], in the piece that holds it.
PrColor prDrawRampColor(const PrDrawRampPiece* piece, const PrColor* colors, float t);

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

// A piece of a border: an area painted with one side's brush and one color.
typedef struct PrDrawBorderPiece {
    PrRectSide side;            // the side whose brush the piece uses
    PrPathFillRule rule;

    // The color that the side's brush multiplies, or that paints alone without one: the side's color, or white at its
    // brush's opacity. Straight alpha, shaded if the style shades the band (PrDrawBorderBand).
    PrColor color;
} PrDrawBorderPiece;

// Called with each piece of a border, in order. The piece's area is `shape`, in the border's space. If `cut` is not
// null, the area is limited to the nonzero interior of `cut`. The polylines are valid only during the call.
typedef void (*PrDrawBorderEmitFn)(void* PR_NULLABLE context, const PrDrawBorderPiece* piece,
                                   const PrPathPolyline* shape, const PrPathPolyline* PR_NULLABLE cut);

// Lowers a border into pieces, in order, and passes each piece to `emit`. The bands between the contours that each
// style names come first. The sides divide the bands along the lines from the outer corners to the inner ones, unless
// the border is uniform (prDrawBorderUniformSide). Dashes and dots come next, placed along each side's length on the
// outer contour. Curves flatten to within `tolerance` in the border's space.
void prDrawBorderLower(const PrDrawBorder* border, float tolerance, PrDrawBorderEmitFn emit,
                       void* PR_NULLABLE context);

// The factor by which a shaded band scales its color channels.
static const float kPrDrawBorderShade = 0.55f;

// A band of one side of a border: the area between the contours at fractions `from` and `to` of the sides' widths, in
// one color. A contour at a fraction is the outer shape inset by that fraction of each side's width.
typedef struct PrDrawBorderBand {
    float from;
    float to;
    PrColor color;              // straight alpha, scaled by kPrDrawBorderShade if the style shades the band
} PrDrawBorderBand;

// The maximum number of bands that one side of a border paints.
enum { kPrDrawBorderMaxBands = 2 };

// Writes the bands that `side` paints to `out`, in order, and returns the number of bands. Returns zero for a side that
// paints nothing, or for a side that is dashed or dotted.
uint32_t prDrawBorderBands(const PrDrawBorder* border, PrRectSide side,
                           PrDrawBorderBand out[PR_NONNULL kPrDrawBorderMaxBands]);

// Returns the side whose style and paint every side shares, if the border is one area: every side paints, and every one
// is solid, or every one double, in one paint. A side that paints nothing, of no width or no style, makes the border
// not one area. Returns null otherwise, and then each side paints its bands between the lines from the corners of the
// outer contour to the corners of the contour at 1.
const PrDrawBorderSide* PR_NULLABLE prDrawBorderUniformSide(const PrDrawBorder* border);

// Returns the fitted shape of the border's contour at `fraction` of the sides' widths. A side that paints nothing
// counts as having zero width.
PrCornerRect prDrawBorderContour(const PrDrawBorder* border, float fraction);

// How a border's sides color its ring, the area between its contours at 0 and 1, as one area. A point of the ring takes
// the color of the side that it lies on. The lines through `split` divide the ring into its corners' quarters, and in
// each quarter the line from the corner of the outer contour to the corner of the contour at 1 divides the side before
// the corner from the side after it, clockwise. A side that paints nothing takes its neighbor's color at the corner, so
// that none of it blends into the neighbor's edge. A backend covers the ring as one area and weighs the two colors on
// either side of the line by the area of the pixel on each (prDrawHalfPlaneArea), so that the sides meet without a
// seam.
typedef struct PrDrawBorderSides {
    PrColor colors[PrRectCorner_Count][2];  // the sides before and after each corner, straight alpha

    // Each corner's line (a, b, c), where a x + b y + c is proportional to the distance from the line, positive toward
    // the side after the corner. A corner where no side has width has a line of zeros.
    float lines[PrRectCorner_Count][3];

    PrVec2 split;
} PrDrawBorderSides;

// Writes how the border's sides color its ring to `out` and returns true, if the border is not one area
// (prDrawBorderUniformSide), each side that paints is one band of a color without a brush, and the bands of opposite
// sides do not overlap. Returns false otherwise, and then the border paints as prDrawBorderLower lowers it.
bool prDrawBorderSides(const PrDrawBorder* border, PrDrawBorderSides* out);

// Returns the corner, by PrRectCorner, whose quarter of the sides' ring holds `point`.
uint32_t prDrawBorderSidesCorner(const PrDrawBorderSides* sides, PrVec2 point);

// Returns the area of a target pixel on the positive side of a line. `distance` is a measure of the pixel's center from
// the line, zero on it and positive on that side, which changes by `gradient` per target pixel along each axis; it need
// not be in pixels. A backend weighs the colors on either side of a line by it, so that the line antialiases as an edge
// of the same angle covers. A measure that does not change across pixels puts the pixel wholly on one side.
float prDrawHalfPlaneArea(float distance, PrVec2 gradient);

// Which side of its shape a shadow falls on.
enum {
    PrDrawShadowKind_Drop,      // outside the caster, cast by it
    PrDrawShadowKind_Inner,     // inside the caster, cast by the outside of its opening
};
typedef uint8_t PrDrawShadowKind;

// A shadow's shape before its blur, in the shadow's space, and the blur's sigma. The shape of a non-scaling shadow
// leaves out the offset, which is in target pixels.
typedef struct PrDrawShadowGeometry {
    PrCornerRect shape;
    float sigma;
} PrDrawShadowGeometry;

// Returns the shadow's shape and sigma. A drop shadow's shape is the caster, offset and grown by the spread. An inner
// shadow's shape is the opening, offset and shrunk by the spread. Corner radii grow and shrink with the spread.
PrDrawShadowGeometry prDrawShadowGeometry(const PrDrawShadow* shadow, PrDrawShadowKind kind);

// Writes a shadow's coverage to `out`, whose pixel (0, 0) is the target pixel at `origin`. A drop shadow is `shape`
// blurred by a Gaussian of `sigma` target pixels, and cut to outside `caster`. An inner shadow blurs what lies outside
// `shape` instead, and is cut to inside `caster`. Both polylines are in target pixels. A sigma above
// kPrDrawBlurMaxSigma blurs at a lower resolution, and is sampled back between its pixels. `rasterizer` is scratch.
void prDrawShadowCoverage(PrPathRasterizer* rasterizer, const PrPathPolyline* shape, const PrPathPolyline* caster,
                          PrDrawShadowKind kind, float sigma, PrPathCoverage coverage, PrIVec2 origin, PrPixels out);

// Appends the decoration's area to `out`, as a polyline that fills with the nonzero rule. Curves flatten to within
// `tolerance` in the decoration's space. A decoration has no area unless its length and thickness are finite and above
// zero.
void prDrawDecorationOutline(const PrDrawDecoration* decoration, float tolerance, PrPathPolyline* out);

// ---------------------------------------------------------------------------------------------------------------------
// Images

// A rectangle of a source, and the rectangle that it draws to.
typedef struct PrDrawImageRect {
    PrRect source;
    PrRect destination;
} PrDrawImageRect;

// Computes the rectangles that a nine-slice draws, in order: the corners, the edges tiled by their modes, then the
// center if it paints. Writes up to `capacity` rectangles to `out` and returns the total number.
uint32_t prDrawNineSliceRects(const PrDrawNineSlice* slices, PrVec2 imageExtent, PrRect destination,
                              PrDrawImageRect* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Layers

// The largest sigma, in pixels, that a blur computes at full resolution. A wider blur computes at the resolution that
// reduces its sigma to this value.
enum { kPrDrawBlurMaxSigma = 16 };

// The maximum distance, in pixels, that a group's filters reach beyond the group at full resolution.
enum { kPrDrawFilterMaxReach = 1024 };

// Returns the resolution of a group's layer, as a fraction of the target's resolution. It is one if `reach`, in target
// pixels, is within kPrDrawFilterMaxReach. Otherwise it is the resolution that reduces the reach to that limit. A
// backend draws a group whose resolution is below one into a layer at that resolution, and resamples the layer as it
// composites.
float prDrawLayerResolution(float reach);

// Returns the extent in texels of the layer that a list's area of `extent` renders into, if a unit of the area covers
// `scale` target pixels along each axis. A layer wider or taller than `limit` keeps its aspect ratio at the largest
// extent that fits, and sampling stretches it. Returns zero by zero for an extent or scale with no area, which has no
// layer.
PrUVec2 prDrawLayerExtent(PrVec2 extent, PrVec2 scale, uint32_t limit);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
