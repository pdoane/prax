// Layout units: the fixed-point distance that layout measures in, and vectors and rectangles of it.
//
// All geometry that layout produces is in units. Sums are exact, so offsets do not drift down a long document, and
// equal geometry is equal in bits, which caches rely on. C++ has saturating operators for them.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Units

// A distance in layout's unit, 1/64 of a pixel (26.6 fixed point): about ±33.5M pixels, the whole range of int32_t,
// saturating at its ends.
typedef struct PrLayoutUnit {
    int32_t raw;
} PrLayoutUnit;

// The units in a pixel.
enum { kPrLayoutUnitsPerPx = 64 };

// Returns `px` pixels in units, truncated toward zero, saturating at the ends of the range; NaN is zero. As an
// initializer, PR_LAYOUT_UNIT(px) gives the same for a constant. The double form keeps the precision of large offsets.
static inline PrLayoutUnit prLayoutUnitFromPxDouble(double px) {
    double units = px * PR_VALUE_CAST(double, kPrLayoutUnitsPerPx);
    PrLayoutUnit unit;
    if (units != units)
        unit.raw = 0;
    else if (units >= 2147483647.0)
        unit.raw = 2147483647;
    else if (units <= -2147483648.0)
        unit.raw = -2147483647 - 1;
    else
        unit.raw = PR_VALUE_CAST(int32_t, units);
    return unit;
}
static inline PrLayoutUnit prLayoutUnitFromPx(float px) {
    return prLayoutUnitFromPxDouble(PR_VALUE_CAST(double, px));
}
#define PR_LAYOUT_UNIT(px) {PR_VALUE_CAST(int32_t, (px) * PR_VALUE_CAST(int32_t, kPrLayoutUnitsPerPx))}

// Returns `px` pixels in whole units rounded down, up, or to the nearest, halfway away from zero, saturating as
// prLayoutUnitFromPx does.
static inline PrLayoutUnit prLayoutUnitFromPxFloorDouble(double px) {
    double scale = PR_VALUE_CAST(double, kPrLayoutUnitsPerPx);
    return prLayoutUnitFromPxDouble(__builtin_floor(px * scale) / scale);
}
static inline PrLayoutUnit prLayoutUnitFromPxCeilDouble(double px) {
    double scale = PR_VALUE_CAST(double, kPrLayoutUnitsPerPx);
    return prLayoutUnitFromPxDouble(__builtin_ceil(px * scale) / scale);
}
static inline PrLayoutUnit prLayoutUnitFromPxRoundDouble(double px) {
    double scale = PR_VALUE_CAST(double, kPrLayoutUnitsPerPx);
    return prLayoutUnitFromPxDouble(__builtin_round(px * scale) / scale);
}
static inline PrLayoutUnit prLayoutUnitFromPxFloor(float px) {
    return prLayoutUnitFromPxFloorDouble(PR_VALUE_CAST(double, px));
}
static inline PrLayoutUnit prLayoutUnitFromPxCeil(float px) {
    return prLayoutUnitFromPxCeilDouble(PR_VALUE_CAST(double, px));
}
static inline PrLayoutUnit prLayoutUnitFromPxRound(float px) {
    return prLayoutUnitFromPxRoundDouble(PR_VALUE_CAST(double, px));
}

// Returns a unit's value in pixels.
static inline float prLayoutUnitToPx(PrLayoutUnit unit) {
    return PR_VALUE_CAST(float, unit.raw) / PR_VALUE_CAST(float, kPrLayoutUnitsPerPx);
}
static inline double prLayoutUnitToPxDouble(PrLayoutUnit unit) {
    return PR_VALUE_CAST(double, unit.raw) / PR_VALUE_CAST(double, kPrLayoutUnitsPerPx);
}

// ---------------------------------------------------------------------------------------------------------------------
// Vectors and rectangles

// A point, an offset or an extent, in units.
typedef struct PrLayoutVec {
    PrLayoutUnit x;
    PrLayoutUnit y;
} PrLayoutVec;

// A rectangle in units, from its left and top edges to its right and bottom ones.
typedef struct PrLayoutRect {
    PrLayoutUnit left;
    PrLayoutUnit top;
    PrLayoutUnit right;
    PrLayoutUnit bottom;
} PrLayoutRect;

// Returns a vector or rectangle of pixels in units, as prLayoutUnitFromPx converts each component, or a vector or
// rectangle of units in pixels.
static inline PrLayoutVec prLayoutVecFromPx(PrVec2 px) {
    PrLayoutVec vec;
    vec.x = prLayoutUnitFromPx(px.x);
    vec.y = prLayoutUnitFromPx(px.y);
    return vec;
}
static inline PrVec2 prLayoutVecToPx(PrLayoutVec vec) {
    PrVec2 px;
    px.x = prLayoutUnitToPx(vec.x);
    px.y = prLayoutUnitToPx(vec.y);
    return px;
}
static inline PrRect prLayoutRectToPx(PrLayoutRect rect) {
    PrRect px;
    px.left = prLayoutUnitToPx(rect.left);
    px.top = prLayoutUnitToPx(rect.top);
    px.right = prLayoutUnitToPx(rect.right);
    px.bottom = prLayoutUnitToPx(rect.bottom);
    return px;
}
static inline PrLayoutRect prLayoutRectFromPx(PrRect px) {
    PrLayoutRect rect;
    rect.left = prLayoutUnitFromPx(px.left);
    rect.top = prLayoutUnitFromPx(px.top);
    rect.right = prLayoutUnitFromPx(px.right);
    rect.bottom = prLayoutUnitFromPx(px.bottom);
    return rect;
}

// Returns the width and height of `rect`.
static inline PrLayoutVec prLayoutRectExtent(PrLayoutRect rect) {
    PrLayoutVec extent;
    extent.x.raw = rect.right.raw - rect.left.raw;
    extent.y.raw = rect.bottom.raw - rect.top.raw;
    return extent;
}

#if defined(__cplusplus)
}
#endif

#if defined(__cplusplus)

// ---------------------------------------------------------------------------------------------------------------------
// C++ operators

// Returns a raw value clamped to the range of a unit, as the operators below saturate.
constexpr PrLayoutUnit prLayoutUnitClamped(int64_t raw) {
    return PrLayoutUnit{raw > INT32_MAX ? INT32_MAX : raw < INT32_MIN ? INT32_MIN : static_cast<int32_t>(raw)};
}

// The operators of units, each saturating at the ends of the range. A sum costs an add and a branch on its overflow,
// which is never taken in practice.
inline PrLayoutUnit operator+(PrLayoutUnit a, PrLayoutUnit b) {
#if defined(__clang__) || defined(__GNUC__)
    PrLayoutUnit sum;
    if (__builtin_add_overflow(a.raw, b.raw, &sum.raw))
        sum.raw = b.raw > 0 ? INT32_MAX : INT32_MIN;
    return sum;
#else
    return prLayoutUnitClamped(int64_t(a.raw) + b.raw);
#endif
}
inline PrLayoutUnit operator-(PrLayoutUnit a, PrLayoutUnit b) {
#if defined(__clang__) || defined(__GNUC__)
    PrLayoutUnit difference;
    if (__builtin_sub_overflow(a.raw, b.raw, &difference.raw))
        difference.raw = b.raw < 0 ? INT32_MAX : INT32_MIN;
    return difference;
#else
    return prLayoutUnitClamped(int64_t(a.raw) - b.raw);
#endif
}
inline PrLayoutUnit operator-(PrLayoutUnit a) {
    return PrLayoutUnit{a.raw == INT32_MIN ? INT32_MAX : -a.raw};
}
inline PrLayoutUnit operator*(PrLayoutUnit a, int32_t n) {
    return prLayoutUnitClamped(int64_t(a.raw) * n);
}
inline PrLayoutUnit operator/(PrLayoutUnit a, int32_t n) {
    return PrLayoutUnit{a.raw / n};
}
inline PrLayoutUnit& operator+=(PrLayoutUnit& a, PrLayoutUnit b) {
    return a = a + b;
}
inline PrLayoutUnit& operator-=(PrLayoutUnit& a, PrLayoutUnit b) {
    return a = a - b;
}
inline bool operator==(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw == b.raw;
}
inline bool operator!=(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw != b.raw;
}
inline bool operator<(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw < b.raw;
}
inline bool operator<=(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw <= b.raw;
}
inline bool operator>(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw > b.raw;
}
inline bool operator>=(PrLayoutUnit a, PrLayoutUnit b) {
    return a.raw >= b.raw;
}

// The operators of vectors of units, each saturating at the ends of the range.
inline PrLayoutVec operator+(PrLayoutVec a, PrLayoutVec b) {
    return PrLayoutVec{a.x + b.x, a.y + b.y};
}
inline PrLayoutVec operator-(PrLayoutVec a, PrLayoutVec b) {
    return PrLayoutVec{a.x - b.x, a.y - b.y};
}
inline PrLayoutVec operator-(PrLayoutVec a) {
    return PrLayoutVec{-a.x, -a.y};
}
inline PrLayoutVec& operator+=(PrLayoutVec& a, PrLayoutVec b) {
    return a = a + b;
}
inline PrLayoutVec& operator-=(PrLayoutVec& a, PrLayoutVec b) {
    return a = a - b;
}
inline bool operator==(PrLayoutVec a, PrLayoutVec b) {
    return a.x == b.x && a.y == b.y;
}
inline bool operator!=(PrLayoutVec a, PrLayoutVec b) {
    return !(a == b);
}
inline bool operator==(PrLayoutRect a, PrLayoutRect b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
inline bool operator!=(PrLayoutRect a, PrLayoutRect b) {
    return !(a == b);
}

#endif

PR_ASSUME_NONNULL_END
