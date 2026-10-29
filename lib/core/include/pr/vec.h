// Vectors: coordinates in the plane or in space, such as a point, an offset, radii or an extent.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN
PR_NO_PADDING_BEGIN

// In C++, vectors add, subtract, multiply and divide component-wise, multiply and divide by a scalar, and compare with
// `==`. Signed vectors negate. The operators are friends in each record, which stays an aggregate.
#if defined(__cplusplus)
#define PR_VEC2_OPERATORS(Type, Scalar)                                                                               \
    friend constexpr Type operator+(Type a, Type b) { return {a.x + b.x, a.y + b.y}; }                                \
    friend constexpr Type operator-(Type a, Type b) { return {a.x - b.x, a.y - b.y}; }                                \
    friend constexpr Type operator*(Type a, Type b) { return {a.x * b.x, a.y * b.y}; }                                \
    friend constexpr Type operator/(Type a, Type b) { return {a.x / b.x, a.y / b.y}; }                                \
    friend constexpr Type operator*(Type a, Scalar s) { return {a.x * s, a.y * s}; }                                  \
    friend constexpr Type operator*(Scalar s, Type a) { return {s * a.x, s * a.y}; }                                  \
    friend constexpr Type operator/(Type a, Scalar s) { return {a.x / s, a.y / s}; }                                  \
    friend constexpr Type& operator+=(Type& a, Type b) { return a = a + b; }                                          \
    friend constexpr Type& operator-=(Type& a, Type b) { return a = a - b; }                                          \
    friend constexpr Type& operator*=(Type& a, Type b) { return a = a * b; }                                          \
    friend constexpr Type& operator/=(Type& a, Type b) { return a = a / b; }                                          \
    friend constexpr Type& operator*=(Type& a, Scalar s) { return a = a * s; }                                        \
    friend constexpr Type& operator/=(Type& a, Scalar s) { return a = a / s; }                                        \
    friend constexpr bool operator==(Type a, Type b) { return a.x == b.x && a.y == b.y; }
#define PR_VEC3_OPERATORS(Type, Scalar)                                                                               \
    friend constexpr Type operator+(Type a, Type b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }                     \
    friend constexpr Type operator-(Type a, Type b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }                     \
    friend constexpr Type operator*(Type a, Type b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }                     \
    friend constexpr Type operator/(Type a, Type b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }                     \
    friend constexpr Type operator*(Type a, Scalar s) { return {a.x * s, a.y * s, a.z * s}; }                         \
    friend constexpr Type operator*(Scalar s, Type a) { return {s * a.x, s * a.y, s * a.z}; }                         \
    friend constexpr Type operator/(Type a, Scalar s) { return {a.x / s, a.y / s, a.z / s}; }                         \
    friend constexpr Type& operator+=(Type& a, Type b) { return a = a + b; }                                          \
    friend constexpr Type& operator-=(Type& a, Type b) { return a = a - b; }                                          \
    friend constexpr Type& operator*=(Type& a, Type b) { return a = a * b; }                                          \
    friend constexpr Type& operator/=(Type& a, Type b) { return a = a / b; }                                          \
    friend constexpr Type& operator*=(Type& a, Scalar s) { return a = a * s; }                                        \
    friend constexpr Type& operator/=(Type& a, Scalar s) { return a = a / s; }                                        \
    friend constexpr bool operator==(Type a, Type b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
// Negation, apart from the rest because an unsigned vector does not negate.
#define PR_VEC2_NEGATE(Type) \
    friend constexpr Type operator-(Type a) { return {-a.x, -a.y}; }
#define PR_VEC3_NEGATE(Type) \
    friend constexpr Type operator-(Type a) { return {-a.x, -a.y, -a.z}; }
#else
#define PR_VEC2_OPERATORS(Type, Scalar)
#define PR_VEC2_NEGATE(Type)
#define PR_VEC3_OPERATORS(Type, Scalar)
#define PR_VEC3_NEGATE(Type)
#endif

// A vector of two floats.
typedef struct PrVec2 {
    float x;
    float y;
    PR_VEC2_OPERATORS(PrVec2, float)
    PR_VEC2_NEGATE(PrVec2)
} PrVec2;

// A vector of three floats.
typedef struct PrVec3 {
    float x;
    float y;
    float z;
    PR_VEC3_OPERATORS(PrVec3, float)
    PR_VEC3_NEGATE(PrVec3)
} PrVec3;

// A vector of two doubles, for points mapped through transforms in doubles.
typedef struct PrDVec2 {
    double x;
    double y;
    PR_VEC2_OPERATORS(PrDVec2, double)
    PR_VEC2_NEGATE(PrDVec2)
} PrDVec2;

// A vector of two signed integers, such as a pixel or an offset in whole pixels.
typedef struct PrIVec2 {
    int32_t x;
    int32_t y;
    PR_VEC2_OPERATORS(PrIVec2, int32_t)
    PR_VEC2_NEGATE(PrIVec2)
} PrIVec2;

// A vector of two unsigned integers, such as an extent in whole pixels or a texel of a texture.
typedef struct PrUVec2 {
    uint32_t x;
    uint32_t y;
    PR_VEC2_OPERATORS(PrUVec2, uint32_t)
} PrUVec2;

// The operator macros serve only the records above.
#undef PR_VEC2_OPERATORS
#undef PR_VEC2_NEGATE
#undef PR_VEC3_OPERATORS
#undef PR_VEC3_NEGATE

PR_NO_PADDING_END

#if defined(__cplusplus)
extern "C" {
#endif

// Returns `vector` scaled to a length of 1, or the zero vector for a vector whose length is not greater than zero.
PrVec2 prVec2Normalize(PrVec2 vector);

#if defined(__cplusplus)
}
#endif

// Returns the point `t` of the way from `a` to `b`: `a` at 0 and `b` at 1.
static inline PrVec2 prVec2Lerp(PrVec2 a, PrVec2 b, float t) {
    return PR_LITERAL(PrVec2){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

PR_ASSUME_NONNULL_END
