// Geometry interfaces
//
// geometry-1 - https://drafts.csswg.org/geometry/

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A 4x4 matrix (geometry-1/#dommatrixreadonly).
typedef struct PrDomMatrix {
    // The elements m11, m12, m13, m14, m21 and so on to m44, in the order toFloat64Array returns them. The 2D elements
    // a to f are m11, m12, m21, m22, m41 and m42.
    double m[16];

    bool is2D;
} PrDomMatrix;

// A point in homogeneous coordinates (geometry-1/#dompointreadonly).
typedef struct PrDomPoint {
    double x;
    double y;
    double z;
    double w;
} PrDomPoint;

// A rectangle (geometry-1/#domrectreadonly): an origin and a size. Either dimension may be negative.
typedef struct PrDomRect {
    double x;
    double y;
    double width;
    double height;
} PrDomRect;

// A quadrilateral (geometry-1/#domquad).
typedef struct PrDomQuad {
    PrDomPoint p[4];                        // the corners p1 to p4
} PrDomQuad;

// ---------------------------------------------------------------------------------------------------------------------
// Making a matrix

// Returns the identity matrix, which is 2D.
PrDomMatrix prDomMatrixIdentity(void);

// Creates the matrix of `count` values (geometry-1/#create-a-dommatrix-from-the-2d-dictionary for a sequence). Six
// values make a 2D matrix of a to f, and sixteen make a 3D matrix of m11 to m44. Returns InvalidData for any other
// count.
PrStatus prDomMatrixFromValues(const double* PR_NULLABLE values, uint32_t count, PrDomMatrix* out);

// ---------------------------------------------------------------------------------------------------------------------
// Transforming a matrix in place
//
// Each function implements the DOMMatrix method of the same name.

// Post-multiplies by `other` (geometry-1/#dom-dommatrix-multiplyself).
void prDomMatrixMultiply(PrDomMatrix* self, const PrDomMatrix* other);

// Pre-multiplies by `other` (geometry-1/#dom-dommatrix-premultiplyself).
void prDomMatrixPreMultiply(PrDomMatrix* self, const PrDomMatrix* other);

// Translates by (tx, ty, tz) (geometry-1/#dom-dommatrix-translateself).
void prDomMatrixTranslate(PrDomMatrix* self, double tx, double ty, double tz);

// Scales about the origin (ox, oy, oz) (geometry-1/#dom-dommatrix-scaleself).
void prDomMatrixScale(PrDomMatrix* self, double sx, double sy, double sz, double ox, double oy, double oz);

// Rotates about z, then y, then x, by angles in degrees (geometry-1/#dom-dommatrix-rotateself).
void prDomMatrixRotate(PrDomMatrix* self, double x, double y, double z);

// Rotates by the angle of the vector (x, y) (geometry-1/#dom-dommatrix-rotatefromvectorself).
void prDomMatrixRotateFromVector(PrDomMatrix* self, double x, double y);

// Rotates by `angle` degrees about the axis (x, y, z) (geometry-1/#dom-dommatrix-rotateaxisangleself).
void prDomMatrixRotateAxisAngle(PrDomMatrix* self, double x, double y, double z, double angle);

// Skews along x or y by an angle in degrees (geometry-1/#dom-dommatrix-skewxself).
void prDomMatrixSkewX(PrDomMatrix* self, double degrees);
void prDomMatrixSkewY(PrDomMatrix* self, double degrees);

// Inverts the matrix. If it has no inverse, sets every element to NaN and clears `is2D`
// (geometry-1/#dom-dommatrix-invertself). Returns true if it had an inverse.
bool prDomMatrixInvert(PrDomMatrix* self);

// Sets element `index` of `m`, following the DOMMatrix attribute setters. Clears `is2D` for a value that a 2D matrix
// cannot hold at that index.
void prDomMatrixSetElement(PrDomMatrix* self, uint32_t index, double value);

// ---------------------------------------------------------------------------------------------------------------------
// Reading a matrix

// Returns true if the matrix is the identity (geometry-1/#dom-dommatrixreadonly-isidentity).
bool prDomMatrixIsIdentity(const PrDomMatrix* self);

// ---------------------------------------------------------------------------------------------------------------------
// Points, rectangles and quadrilaterals

// Returns `self` transformed by `matrix` (geometry-1/#transform-a-point-with-a-matrix).
PrDomPoint prDomPointMatrixTransform(const PrDomPoint* self, const PrDomMatrix* matrix);

// Return the edges of a rectangle, whatever the sign of its size (geometry-1/#dom-domrectreadonly-top). Top and bottom
// are NaN if y or the height is NaN. Right and left are NaN if x or the width is NaN.
double prDomRectTop(const PrDomRect* self);
double prDomRectRight(const PrDomRect* self);
double prDomRectBottom(const PrDomRect* self);
double prDomRectLeft(const PrDomRect* self);

// Returns the quadrilateral of a rectangle's corners, clockwise from its origin (geometry-1/#dom-domquad-fromrect).
PrDomQuad prDomQuadFromRect(const PrDomRect* rect);

// Returns the smallest rectangle that contains every corner. Each coordinate is NaN if that coordinate of any corner is
// NaN (geometry-1/#dom-domquad-getbounds).
PrDomRect prDomQuadGetBounds(const PrDomQuad* self);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
