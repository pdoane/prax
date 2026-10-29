// Rectangles and transforms in two dimensions.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN
PR_NO_PADDING_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An axis-aligned rectangle by its edges. It is empty if `right` <= `left` or `bottom` <= `top`.
typedef struct PrRect {
    float left;
    float top;
    float right;
    float bottom;
} PrRect;

// An axis-aligned rectangle of integers by its edges, such as whole pixels or texels. It is empty if `right` <= `left`
// or `bottom` <= `top`.
typedef struct PrIRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} PrIRect;

// The corners of a rectangle, clockwise from the top left.
enum {
    PrRectCorner_TopLeft,
    PrRectCorner_TopRight,
    PrRectCorner_BottomRight,
    PrRectCorner_BottomLeft,
    PrRectCorner_Count,
};
typedef uint8_t PrRectCorner;

// The sides of a rectangle, clockwise from the top. Insets and widths are given in this order.
enum {
    PrRectSide_Top,
    PrRectSide_Right,
    PrRectSide_Bottom,
    PrRectSide_Left,
    PrRectSide_Count,
};
typedef uint8_t PrRectSide;

// A shaped corner. Its curve spans the box of `radii`, and `shape` is K in CSS `corner-shape: superellipse(K)`.
//
// A K of zero or more is a quarter of the superellipse of exponent 2^K: 1 is round, 2 a squircle, 0 a bevel and
// +infinity square. A negative K is the corner of -K reflected into the box, which is concave: -1 is a scoop and
// -infinity a notch. A corner whose radii have an axis of zero or less is square, regardless of K.
typedef struct PrCorner {
    PrVec2 radii;      // horizontal and vertical half-axes
    float shape;       // K
} PrCorner;

// A rectangle with shaped corners, indexed by PrRectCorner.
typedef struct PrCornerRect {
    PrRect rect;
    PrCorner corners[PrRectCorner_Count];
} PrCornerRect;

// An affine transform of the plane: (x, y) maps to (a x + c y + e, b x + d y + f). The fields are the top two rows of a
// 3 by 3 matrix.
typedef struct PrAffine2 {
    float a;
    float b;
    float c;
    float d;
    float e;
    float f;
} PrAffine2;

// An affine transform of the plane in doubles, with the fields of PrAffine2. It composes transforms with large
// translations, such as the offsets and scrolling of a long document, without losing the precision that small
// translations keep.
typedef struct PrDAffine2 {
    double a;
    double b;
    double c;
    double d;
    double e;
    double f;
} PrDAffine2;

// An affine transform in four dimensions, as the top four rows of a 5 by 5 matrix. `m` is indexed [row][column] and
// maps column vectors (x, y, z, w, 1). A color matrix is a PrAffine4 that acts on the four channels of a color.
typedef struct PrAffine4 {
    float m[4][5];
} PrAffine4;

// A projective transform of the plane. `m` is indexed [row][column] and maps column vectors (x, y, 1).
typedef struct PrMatrix3 {
    float m[3][3];
} PrMatrix3;

// A transform in three dimensions. `m` is indexed [row][column] and maps column vectors (x, y, z, w).
typedef struct PrMatrix4 {
    float m[4][4];
} PrMatrix4;

// A projective transform of the plane in doubles, with the fields of PrMatrix3, and a transform in three dimensions in
// doubles, with the fields of PrMatrix4. They compose long chains of transforms, such as the transforms and offsets
// between a box deep in a document and the viewport, without losing the precision that small translations keep.
typedef struct PrDMatrix3 {
    double m[3][3];
} PrDMatrix3;
typedef struct PrDMatrix4 {
    double m[4][4];
} PrDMatrix4;

// ---------------------------------------------------------------------------------------------------------------------
// Rectangles

// Returns the rectangle of `extent` with its top left at `origin`.
PrRect prRectAt(PrVec2 origin, PrVec2 extent);

// Returns true if `rect` has no area.
bool prRectIsEmpty(PrRect rect);

// Return the width and height of `rect`. Each is zero if the far edge is not past the near one.
float prRectWidth(PrRect rect);
float prRectHeight(PrRect rect);

// Returns the center of `rect`.
PrVec2 prRectCenter(PrRect rect);

// Returns the point of `rect` at `corner`.
PrVec2 prRectCornerPoint(PrRect rect, PrRectCorner corner);

// Returns the smallest rectangle that contains `a` and `b`. An empty rectangle contains nothing.
PrRect prRectUnion(PrRect a, PrRect b);

// Returns the overlap of `a` and `b`, which is empty if they do not overlap.
PrRect prRectIntersect(PrRect a, PrRect b);

// The distance in pixels that an edge may be from a whole pixel and still lie on it.
static const float kPrRectOnPixelTolerance = 1e-3f;

// Returns true if every edge of `rect` lies within kPrRectOnPixelTolerance of a whole pixel.
bool prRectIsOnPixels(PrRect rect);

// ---------------------------------------------------------------------------------------------------------------------
// Integer rectangles
//
// A conversion from PrRect clamps coordinates to within kPrIRectMaxCoordinate of the origin, so that arithmetic on
// edges and widths does not overflow.

// The farthest distance from the origin at which a conversion from PrRect places an edge.
enum { kPrIRectMaxCoordinate = 1 << 28 };

// Returns true if `rect` has no area.
bool prIRectIsEmpty(PrIRect rect);

// Return the width and height of `rect`. Each is zero if the far edge is not past the near one.
uint32_t prIRectWidth(PrIRect rect);
uint32_t prIRectHeight(PrIRect rect);

// Returns the pixels that `rect` covers. An edge that reaches kPrRectOnPixelTolerance or less into a pixel does not
// cover it. The result is empty for an empty rectangle.
PrIRect prIRectRoundOut(PrRect rect);

// Returns the pixels whose centers `rect` contains. The result is empty for an empty rectangle.
PrIRect prIRectRound(PrRect rect);

// Returns `rect` in float coordinates.
PrRect prRectFromIRect(PrIRect rect);

// Returns the rectangle of `extent` with its top left at `origin`.
PrIRect prIRectAt(PrIVec2 origin, PrUVec2 extent);

// Returns the smallest rectangle that contains `a` and `b`. An empty rectangle contains nothing.
PrIRect prIRectUnion(PrIRect a, PrIRect b);

// Returns the overlap of `a` and `b`, which is empty if they do not overlap.
PrIRect prIRectIntersect(PrIRect a, PrIRect b);

// Returns `rect` with each edge moved outward by `by`.
PrIRect prIRectOutset(PrIRect rect, int32_t by);

// Returns true if `outer` contains every pixel of `inner`. Returns false if `inner` is empty.
bool prIRectContains(PrIRect outer, PrIRect inner);

// ---------------------------------------------------------------------------------------------------------------------
// Corners

// Returns true if `corner` is square, which means that its radii have an axis of zero or less, or its K is +infinity.
bool prCornerIsSquare(PrCorner corner);

// Returns the point at `t` along the curve of a corner whose K is `shape`, as fractions of the corner's radii. `t` runs
// from 0 at the side before the corner to 1 at the side after. x runs from the start of the curve along the side
// before, and y runs inward along the side after.
//
// With p = 2^(1-|K|), the point is (sin(πt/2)^p, 1-cos(πt/2)^p) for K >= 0, and (1-cos(πt/2)^p, sin(πt/2)^p) for K < 0.
PrVec2 prCornerCurvePoint(float shape, float t);

// Returns the direction of the curve at `t`, in the frame of prCornerCurvePoint. It is finite at both ends.
PrVec2 prCornerCurveTangent(float shape, float t);

// Returns a round corner with `radii`.
PrCorner prCornerRound(PrVec2 radii);

// ---------------------------------------------------------------------------------------------------------------------
// Rectangles with shaped corners

// Returns `rect` with every corner round, with `radius` on both axes.
PrCornerRect prCornerRectRound(PrRect rect, float radius);

// Returns the shape as it is drawn. A corner whose radii have an axis of zero or less becomes square. Every radius is
// then scaled by one factor, at most 1, so that the two radii along each side sum to no more than the side's length.
// Last, every concave corner is scaled by a further factor until no two opposite corners have overlapping hulls.
PrCornerRect prCornerRectFit(PrCornerRect shape);

// Returns the interior of the fitted `shape` after its sides move inward by `insets`, in PrRectSide order. Each corner
// keeps its K.
//
// A convex corner's radii shrink until the midpoint of its curve has moved inward by the insets across the corner's
// diagonal. The radii stop at zero, which makes the corner square. A round corner's radii therefore shrink by the
// insets, and a bevel moves parallel to itself. A concave corner keeps its radii, and its curve moves inward with the
// sides. If the insets cross, the rectangle is empty.
PrCornerRect prCornerRectInset(PrCornerRect shape, const float insets[PR_NONNULL PrRectSide_Count]);

// Returns the fitted `shape` after its sides move outward by `outsets`, which are zero or more, in PrRectSide order, as
// CSS grows a box by a shadow's spread. Each corner keeps its K, and a square corner stays square. A radius r grows by
// the outset s across it, or by s(1 + (r/s - 1)^3) if r is less than s, so that a small radius grows less than the
// outset.
PrCornerRect prCornerRectOutset(PrCornerRect shape, const float outsets[PR_NONNULL PrRectSide_Count]);

// Returns true if `point` is inside the fitted `shape`, or on its edge.
bool prCornerRectContains(PrCornerRect shape, PrVec2 point);

// A corner of a rectangle with shaped corners, placed on the rectangle: the rectangle's corner, the directions of the
// sides before and after it in clockwise order, and the corner's radius along each.
typedef struct PrCornerPlacement {
    PrVec2 point;
    PrVec2 before;
    PrVec2 after;
    float radiusBefore;
    float radiusAfter;
    float shape;
} PrCornerPlacement;

// Returns `corner` of `shape` placed on its rectangle.
PrCornerPlacement prCornerRectPlacement(PrCornerRect shape, PrRectCorner corner);

// Returns the point of a placed corner at `uv`, the coordinates that prCornerCurvePoint gives: along the side before
// the corner from where its curve starts, and then along the side after it. The curve starts at (0, 0).
PrVec2 prCornerPlacementPoint(PrCornerPlacement placement, PrVec2 uv);

// ---------------------------------------------------------------------------------------------------------------------
// Affine transforms of the plane

// Return the identity, a translation by `offset`, and a scale by `factors`, one along each axis.
PrAffine2 prAffine2Identity(void);
PrAffine2 prAffine2Translate(PrVec2 offset);
PrAffine2 prAffine2Scale(PrVec2 factors);

// Returns a rotation by `radians`, clockwise if y points down. Multiples of a quarter turn are exact.
PrAffine2 prAffine2Rotate(float radians);

// Returns `first` followed by `second`: a point maps through `first` and then through `second`.
PrAffine2 prAffine2Concat(PrAffine2 first, PrAffine2 second);

// Writes the inverse of `transform` to `*out`. Returns false for a singular transform, and leaves `*out` unchanged.
bool prAffine2Invert(PrAffine2 transform, PrAffine2* out);

// Returns `point` mapped through `transform`.
PrVec2 prAffine2MapPoint(PrAffine2 transform, PrVec2 point);

// Returns the bounds of the four corners of `rect` mapped through `transform`. The result is empty for an empty
// rectangle.
PrRect prAffine2MapRect(PrAffine2 transform, PrRect rect);

// Returns the largest factor by which `transform` scales a length.
float prAffine2MaxScale(PrAffine2 transform);

// Returns true if `transform` is exactly the identity.
bool prAffine2IsIdentity(PrAffine2 transform);

// Returns true if `transform` scales every direction equally. Rotations, reflections, uniform scales and translations
// do.
bool prAffine2IsSimilarity(PrAffine2 transform);

// Returns true if `transform` maps axis-aligned rectangles to axis-aligned rectangles. Such a transform scales and
// translates, and may swap the axes.
bool prAffine2IsAxisAligned(PrAffine2 transform);

// ---------------------------------------------------------------------------------------------------------------------
// Affine transforms of the plane in doubles

// Return the identity, and a translation by (`x`, `y`).
PrDAffine2 prDAffine2Identity(void);
PrDAffine2 prDAffine2Translate(double x, double y);

// Convert `transform` to doubles, and round `transform` to floats.
PrDAffine2 prDAffine2FromAffine2(PrAffine2 transform);
PrAffine2 prAffine2FromDAffine2(PrDAffine2 transform);

// Returns `first` followed by `second`: a point maps through `first` and then through `second`.
PrDAffine2 prDAffine2Concat(PrDAffine2 first, PrDAffine2 second);

// Writes the inverse of `transform` to `*out`. Returns false for a singular transform, and leaves `*out` unchanged.
bool prDAffine2Invert(PrDAffine2 transform, PrDAffine2* out);

// ---------------------------------------------------------------------------------------------------------------------
// Transforms in three dimensions

// Returns the identity matrix.
PrMatrix4 prMatrix4Identity(void);

// Returns `transform` acting on x and y, leaving z and w unchanged.
PrMatrix4 prMatrix4FromAffine2(PrAffine2 transform);

// Return a translation by `offset`, and a scale by `factors`, one along each axis.
PrMatrix4 prMatrix4Translate(PrVec3 offset);
PrMatrix4 prMatrix4Scale(PrVec3 factors);

// Returns a rotation by `radians` about `axis`, clockwise looking along the axis if y points down. Returns the identity
// for an axis of zero length.
PrMatrix4 prMatrix4Rotate(PrVec3 axis, float radians);

// Returns a perspective with the eye `distance` from the z = 0 plane, which matches CSS perspective(). Returns the
// identity for a `distance` of zero or less.
PrMatrix4 prMatrix4Perspective(float distance);

// Returns `first` followed by `second`: a point maps through `first` and then through `second`.
PrMatrix4 prMatrix4Concat(const PrMatrix4* first, const PrMatrix4* second);

// Writes the inverse of `matrix` to `*out`. Returns false for a singular matrix, and leaves `*out` unchanged.
bool prMatrix4Invert(const PrMatrix4* matrix, PrMatrix4* out);

// Maps (x, y, 0, 1) of `point` through `matrix`, and writes the mapped x and y, divided by w, to `*out`. Returns false,
// and leaves `*out` unchanged, if w is zero or less, which means that the point is behind the eye.
bool prMatrix4MapPoint(const PrMatrix4* matrix, PrVec2 point, PrVec2* out);

// Writes `matrix` as an affine transform on the z = 0 plane to `*out`. Returns false if the matrix has perspective
// there, and leaves `*out` unchanged.
bool prAffine2FromMatrix4(const PrMatrix4* matrix, PrAffine2* out);

// ---------------------------------------------------------------------------------------------------------------------
// Transforms in three dimensions in doubles

// Returns the identity matrix.
PrDMatrix4 prDMatrix4Identity(void);

// Convert `matrix` to doubles, and round `matrix` to floats.
PrDMatrix4 prDMatrix4FromMatrix4(const PrMatrix4* matrix);
PrMatrix4 prMatrix4FromDMatrix4(const PrDMatrix4* matrix);

// Returns `first` followed by `second`: a point maps through `first` and then through `second`.
PrDMatrix4 prDMatrix4Concat(const PrDMatrix4* first, const PrDMatrix4* second);

// Writes the inverse of `matrix` to `*out`. Returns false for a singular matrix, and leaves `*out` unchanged.
bool prDMatrix4Invert(const PrDMatrix4* matrix, PrDMatrix4* out);

// Returns true if `matrix` turns the z = 0 plane so that the eye, looking down the z axis from positive z, sees the
// plane's back: the plane's normal (0, 0, 1), mapped by the inverse transpose of `matrix`, points away from the eye. A
// mirror within the plane shows its front. A singular matrix shows no back.
bool prDMatrix4ShowsBack(const PrDMatrix4* matrix);

// ---------------------------------------------------------------------------------------------------------------------
// Projective transforms of the plane

// Returns `transform` as a projective transform.
PrMatrix3 prMatrix3FromAffine2(PrAffine2 transform);

// Returns the projective transform that `matrix` applies to the z = 0 plane: its rows and columns for x, y and w.
PrMatrix3 prMatrix3FromMatrix4(const PrMatrix4* matrix);

// Returns true if `matrix` is affine: its last row is (0, 0, w), with w greater than zero.
bool prMatrix3IsAffine(const PrMatrix3* matrix);

// Writes `matrix` as an affine transform to `*out`, divided by its w. Returns false if the matrix is not affine, and
// leaves `*out` unchanged.
bool prAffine2FromMatrix3(const PrMatrix3* matrix, PrAffine2* out);

// Returns the affine transform that agrees with `matrix` to first order at `point`: the tangent of the projection
// there. Returns the zero transform if w at `point` is zero or less.
PrAffine2 prMatrix3AffineNear(const PrMatrix3* matrix, PrVec2 point);

// Returns a bound on the factor by which `matrix` scales a length anywhere in the part of `rect` where w is at least
// `minimumW`, which must be greater than zero. The bound is never below the largest such factor, and equals it if the
// matrix is affine. Returns zero if no part of `rect` has w of at least `minimumW`.
float prMatrix3MaxScale(const PrMatrix3* matrix, PrRect rect, float minimumW);

// Returns `first` followed by `second`: a point maps through `first` and then through `second`.
PrMatrix3 prMatrix3Concat(const PrMatrix3* first, const PrMatrix3* second);

// Maps (x, y, 1) of `point` through `matrix`, and writes the mapped x and y, divided by w, to `*out`. Returns false,
// and leaves `*out` unchanged, if w is zero or less, which means that the point maps to infinity, or lies behind the
// eye of the projection that the matrix carries out or inverts.
bool prMatrix3MapPoint(const PrMatrix3* matrix, PrVec2 point, PrVec2* out);

// Writes the inverse of `matrix`, computed in double precision, to `*out`. Returns false for a singular matrix, and
// leaves `*out` unchanged.
bool prMatrix3Invert(const PrMatrix3* matrix, PrMatrix3* out);

// ---------------------------------------------------------------------------------------------------------------------
// Projective transforms of the plane in doubles

// Returns the identity.
PrDMatrix3 prDMatrix3Identity(void);

// Returns `transform` as a projective transform.
PrDMatrix3 prDMatrix3FromDAffine2(PrDAffine2 transform);

// Convert `matrix` to doubles, and round `matrix` to floats.
PrDMatrix3 prDMatrix3FromMatrix3(const PrMatrix3* matrix);
PrMatrix3 prMatrix3FromDMatrix3(const PrDMatrix3* matrix);

// Returns the projective transform that `matrix` applies to the z = 0 plane: its rows and columns for x, y and w.
PrDMatrix3 prDMatrix3FromDMatrix4(const PrDMatrix4* matrix);

// Writes `matrix` as an affine transform to `*out`, divided by its w. Returns false if its last row is not (0, 0, w)
// with w greater than zero, and leaves `*out` unchanged.
bool prDAffine2FromDMatrix3(const PrDMatrix3* matrix, PrDAffine2* out);

// Returns `first` followed by `second`: a point maps through `first` and then through `second`.
PrDMatrix3 prDMatrix3Concat(const PrDMatrix3* first, const PrDMatrix3* second);

// Maps (x, y, 1) of `point` through `matrix`, and writes the mapped x and y, divided by w, to `*out`. Returns false,
// and leaves `*out` unchanged, if w is zero or less, which means that the point maps to infinity, or lies behind the
// eye of the projection that the matrix carries out or inverts.
bool prDMatrix3MapPoint(const PrDMatrix3* matrix, PrDVec2 point, PrDVec2* out);

// Writes the inverse of `matrix` to `*out`. Returns false for a singular matrix, and leaves `*out` unchanged.
bool prDMatrix3Invert(const PrDMatrix3* matrix, PrDMatrix3* out);

#if defined(__cplusplus)
}
#endif

PR_NO_PADDING_END
PR_ASSUME_NONNULL_END
