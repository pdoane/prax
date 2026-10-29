// Stroking paths: the outline covered by a line of some width along a path.

#pragma once

#include "pr/base.h"
#include "pr/path.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How two segments meet at a turn in a contour, on the outer side of the turn.
enum {
    PrPathJoin_Miter,       // the sides extended to their meeting point, beveled past the limit
    PrPathJoin_MiterClip,   // the miter, cut off at the limit
    PrPathJoin_Round,       // the arc of half the width
    PrPathJoin_Bevel,       // the straight cut between the ends of the sides

    // Each side extended along the circle of its curvature to the point at which the two meet. If they meet past the
    // limit or do not meet, the join is MiterClip.
    PrPathJoin_Arcs,
};
typedef uint8_t PrPathJoin;

// How an open line ends.
enum {
    PrPathCap_Butt,         // at the end point
    PrPathCap_Round,        // a half circle past it
    PrPathCap_Square,       // half the width past it
    PrPathCap_Triangle,     // a point half the width past it
};
typedef uint8_t PrPathCap;

// A point of a width profile. A profile varies the half width of each side along a contour. The half widths interpolate
// linearly between points, and hold beyond the first and last points. Positions do not decrease from one point to the
// next.
typedef struct PrPathWidthPoint {
    float position;         // along the contour, as a fraction of its length
    float left;             // the left half width, as a fraction of the stroke width
    float right;            // the right half width, as a fraction of the stroke width
} PrPathWidthPoint;

// The shape of a stroke. The first and last ends of a contour use `startCap` and `endCap`. The ends that the dash
// pattern creates use `dashCap`. Left and right are as seen facing along the contour. If the contour runs toward +x and
// y points down, left is toward -y.
typedef struct PrPathStroke {
    float width;
    float miterLimit;                            // the longest miter, as a multiple of the width
    const float* PR_NULLABLE dashes;             // dash and gap lengths in turn, from the start of each contour
    uint32_t dashCount;                          // if odd, the list of lengths is used twice over
    float dashOffset;                            // how far into the pattern each contour starts
    const PrPathWidthPoint* PR_NULLABLE profile; // null for half the width on each side
    uint32_t profileCount;
    PrPathJoin join;
    PrPathCap startCap;
    PrPathCap endCap;
    PrPathCap dashCap;

    // The direction that the caps of a contour of zero length face, which need not be a unit vector. A zero vector
    // means +x.
    PrVec2 zeroLengthDirection;
} PrPathStroke;

// The default stroke: a width of one, a miter limit of four, miter joins and butt caps, with no dashes and no profile.
// A contour of zero length faces +x.
PrPathStroke prPathStrokeDefault(void);

// Appends to `out` the outline of `path` stroked as `stroke` specifies. An open contour gives one loop: down one side,
// around the end cap, back along the other side and around the start cap. A closed contour gives two loops wound in
// opposite directions, one per side. A join adds points only on the outer side of a turn, and the inner side folds over
// itself. Filled with the nonzero rule, the outline covers the stroke once, with no seams inside it.
//
// Curves become lines within `tolerance`, which also bounds the error of round joins and caps. Each contour of the path
// restarts the dash pattern. A width of zero or less strokes nothing. A pattern with a negative length, or whose
// lengths sum to zero, strokes solid. A segment of zero length, or a dash of zero length, strokes as a dot if its caps
// have area. A contour that is a move followed by a close has one segment of zero length. A contour that is only a move
// strokes nothing.
void prPathStrokeOutline(const PrPath* path, const PrPathStroke* stroke, float tolerance, PrPathPolyline* out);

// The half widths of a stroke at one place along a contour.
typedef struct PrPathStrokeWidth {
    float left;
    float right;
} PrPathStrokeWidth;

// Returns the half widths of `stroke` at `fraction` of the length of a contour.
PrPathStrokeWidth prPathStrokeWidthAt(const PrPathStroke* stroke, float fraction);

// Returns how far the outline of `stroke` may lie from the stroked path. This is the widest half width, times the
// factor by which the joins and caps may reach past it. Returns zero for a width of zero or less.
float prPathStrokeReach(const PrPathStroke* stroke);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
