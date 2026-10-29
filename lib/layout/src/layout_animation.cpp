// Layout animations: easing, timing, transform function lists, and tracks of composited effects.
//
// css-easing-2 - https://drafts.csswg.org/css-easing-2/
// css-transforms-1 - https://drafts.csswg.org/css-transforms-1/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/

#include "pr/layout_animation.h"

#include "pr/float.h"

namespace pr_impl {

namespace {

double bezierAt(double a, double b, double t) {
    // The coordinate of the curve at t, for a curve whose end points are 0 and 1.
    double u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

double bezierSlope(double a, double b, double t) {
    double u = 1 - t;
    return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
}

// How far the x that the solver finds may lie from the x that it is given.
constexpr double kBezierTolerance = 1e-14;

// Returns the y of the curve at the point whose x is `x`, which is in [0, 1]. Uses Newton's method starting from x, and
// falls back to bisection if Newton's method stalls.
double bezierEase(const PrLayoutEasing& curve, double x) {
    double x1 = curve.x1;
    double x2 = curve.x2;
    double y1 = curve.y1;
    double y2 = curve.y2;
    double t = x;
    for (uint32_t i = 0; i < 16; ++i) {
        double error = bezierAt(x1, x2, t) - x;
        if (absF64(error) < kBezierTolerance)
            return bezierAt(y1, y2, t);
        double slope = bezierSlope(x1, x2, t);
        if (absF64(slope) < 1e-6)
            break;
        t -= error / slope;
    }
    double low = 0;
    double high = 1;
    t = x;
    for (uint32_t i = 0; i < 64; ++i) {
        double at = bezierAt(x1, x2, t);
        if (absF64(at - x) < kBezierTolerance)
            break;
        if (at < x)
            low = t;
        else
            high = t;
        t = (low + high) / 2;
    }
    return bezierAt(y1, y2, t);
}

// css-easing-2/#step-easing-algo
double stepEase(const PrLayoutEasing& easing, double input, bool before) {
    double scaled = input * double(easing.steps);
    double step = floorF64(scaled);
    if (easing.position == PrLayoutStepPosition_JumpStart || easing.position == PrLayoutStepPosition_JumpBoth)
        step += 1;
    if (before && scaled == floorF64(scaled))
        step -= 1;
    if (input >= 0 && step < 0)
        step = 0;
    double jumps = double(easing.steps);
    if (easing.position == PrLayoutStepPosition_JumpBoth)
        jumps += 1;
    else if (easing.position == PrLayoutStepPosition_JumpNone)
        jumps -= 1;
    if (jumps <= 0)
        return input;
    if (input <= 1 && step > jumps)
        step = jumps;
    return step / jumps;
}

double unit(double x) {
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

PrDMatrix4 transposed(const PrDMatrix4& a) {
    PrDMatrix4 out{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            out.m[row][column] = a.m[column][row];
    }
    return out;
}

// Returns true if `a` is a 2D matrix: one that matrix() can write.
bool is2D(const PrDMatrix4& a) {
    return a.m[0][2] == 0 && a.m[1][2] == 0 && a.m[2][0] == 0 && a.m[2][1] == 0 && a.m[2][2] == 1 && a.m[2][3] == 0
           && a.m[3][0] == 0 && a.m[3][1] == 0 && a.m[3][2] == 0 && a.m[3][3] == 1;
}

// Returns the rotation by `angle` about the axis (x, y, z), or the identity for an axis of zero length
// (css-transforms-2/#Rotate3dDefined).
PrDMatrix4 rotation(double x, double y, double z, double angle) {
    PrDMatrix4 out = prDMatrix4Identity();
    double length = sqrtF64(x * x + y * y + z * z);
    if (!(length > 0))
        return out;
    x /= length;
    y /= length;
    z /= length;
    double c = cosF64(angle);
    double s = sinF64(angle);
    // At a multiple of a quarter turn, the sine or cosine that the error of an angle in floats leaves near zero becomes
    // zero, and the other becomes one, so that a whole turn is the identity.
    constexpr double kNearZero = 1e-6;
    if (absF64(s) < kNearZero) {
        s = 0;
        c = c < 0 ? -1 : 1;
    } else if (absF64(c) < kNearZero) {
        c = 0;
        s = s < 0 ? -1 : 1;
    }
    // A rotation about an axis of the coordinate system leaves that axis's elements exactly as in the identity.
    if (x == 0 && y == 0) {
        out.m[0][0] = out.m[1][1] = c;
        out.m[1][0] = s * z;
        out.m[0][1] = -s * z;
        return out;
    }
    if (x == 0 && z == 0) {
        out.m[0][0] = out.m[2][2] = c;
        out.m[2][0] = -s * y;
        out.m[0][2] = s * y;
        return out;
    }
    if (y == 0 && z == 0) {
        out.m[1][1] = out.m[2][2] = c;
        out.m[2][1] = s * x;
        out.m[1][2] = -s * x;
        return out;
    }
    double t = 1 - c;
    out.m[0][0] = t * x * x + c;
    out.m[1][0] = t * x * y + s * z;
    out.m[2][0] = t * x * z - s * y;
    out.m[0][1] = t * x * y - s * z;
    out.m[1][1] = t * y * y + c;
    out.m[2][1] = t * y * z + s * x;
    out.m[0][2] = t * x * z + s * y;
    out.m[1][2] = t * y * z - s * x;
    out.m[2][2] = t * z * z + c;
    return out;
}

// Returns the matrix of a function. `extent` resolves its percentages.
PrDMatrix4 functionMatrix(const PrLayoutTransformFunction& f, PrVec2 extent) {
    double v[16];
    for (uint32_t i = 0; i < 16; ++i)
        v[i] = double(f.values[i]);
    PrDMatrix4 out = prDMatrix4Identity();
    switch (f.kind) {
        case PrLayoutTransformKind_Translate:
            out.m[0][3] = v[0] + v[3] * double(extent.x);
            out.m[1][3] = v[1] + v[4] * double(extent.y);
            out.m[2][3] = v[2];
            break;
        case PrLayoutTransformKind_Scale:
            out.m[0][0] = v[0];
            out.m[1][1] = v[1];
            out.m[2][2] = v[2];
            break;
        case PrLayoutTransformKind_Rotate: return rotation(v[0], v[1], v[2], v[3]);
        case PrLayoutTransformKind_Skew:
            out.m[0][1] = tanF64(v[0]);
            out.m[1][0] = tanF64(v[1]);
            break;
        case PrLayoutTransformKind_Perspective:
            if (v[0] > 0)
                out.m[3][2] = -1 / (v[0] < 1 ? 1 : v[0]);
            break;
        case PrLayoutTransformKind_Matrix:
            for (uint32_t column = 0; column < 4; ++column) {
                for (uint32_t row = 0; row < 4; ++row)
                    out.m[row][column] = v[column * 4 + row];
            }
            break;
        default: break;
    }
    return out;
}

// Returns the matrix that functions [start, end) of `list` compose to.
PrDMatrix4 listMatrix(const PrLayoutTransformList& list, uint32_t start, PrVec2 extent) {
    PrDMatrix4 total = prDMatrix4Identity();
    for (uint32_t i = start; i < list.count; ++i) {
        PrDMatrix4 function = functionMatrix(list.functions[i], extent);
        total = prDMatrix4Concat(&function, &total);
    }
    return total;
}

// Returns a PrDMatrix4 function of `a`.
PrLayoutTransformFunction matrixFunction(const PrDMatrix4& a) {
    PrLayoutTransformFunction f{};
    f.kind = PrLayoutTransformKind_Matrix;
    for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row)
            f.values[column * 4 + row] = float(a.m[row][column]);
    }
    return f;
}

// Returns the identity function of the kind of `like`. A rotation keeps the axis of `like`, which interpolating with a
// rotation by zero uses.
PrLayoutTransformFunction identityLike(const PrLayoutTransformFunction& like) {
    PrLayoutTransformFunction f{};
    f.kind = like.kind;
    switch (like.kind) {
        case PrLayoutTransformKind_Scale: f.values[0] = f.values[1] = f.values[2] = 1; break;
        case PrLayoutTransformKind_Rotate:
            f.values[0] = like.values[0];
            f.values[1] = like.values[1];
            f.values[2] = like.values[2];
            break;
        case PrLayoutTransformKind_Matrix: f.values[0] = f.values[5] = f.values[10] = f.values[15] = 1; break;
        default: break;
    }
    return f;
}

// A 2D matrix decomposed (css-transforms-1/#decomposing-a-2d-matrix). The matrix is the product of translate, rotate,
// the remaining shear and scale, in that order, so scale applies first.
struct Decomposed2D {
    double translateX;
    double translateY;
    double scaleX;
    double scaleY;
    double angle;
    double k[4];                    // the shear, column by column; k[0] is one and k[1] is zero
};

Decomposed2D decompose2D(const PrDMatrix4& m) {
    Decomposed2D d{};
    d.translateX = m.m[0][3];
    d.translateY = m.m[1][3];
    double c0x = m.m[0][0];
    double c0y = m.m[1][0];
    double c1x = m.m[0][1];
    double c1y = m.m[1][1];
    d.scaleX = sqrtF64(c0x * c0x + c0y * c0y);
    d.scaleY = sqrtF64(c1x * c1x + c1y * c1y);
    // A reflection is a negative scale on one axis.
    if (c0x * c1y - c0y * c1x < 0) {
        if (c0x < c1y)
            d.scaleX = -d.scaleX;
        else
            d.scaleY = -d.scaleY;
    }
    if (d.scaleX != 0) {
        c0x /= d.scaleX;
        c0y /= d.scaleX;
    }
    if (d.scaleY != 0) {
        c1x /= d.scaleY;
        c1y /= d.scaleY;
    }
    d.angle = atan2F64(c0y, c0x);
    double cs = cosF64(d.angle);
    double sn = sinF64(d.angle);
    d.k[0] = cs * c0x + sn * c0y;
    d.k[1] = -sn * c0x + cs * c0y;
    d.k[2] = cs * c1x + sn * c1y;
    d.k[3] = -sn * c1x + cs * c1y;
    return d;
}

PrDMatrix4 recompose2D(const Decomposed2D& d) {
    double cs = cosF64(d.angle);
    double sn = sinF64(d.angle);
    // Multiplies rotate by shear, then scales each column.
    PrDMatrix4 out = prDMatrix4Identity();
    out.m[0][0] = (cs * d.k[0] - sn * d.k[1]) * d.scaleX;
    out.m[1][0] = (sn * d.k[0] + cs * d.k[1]) * d.scaleX;
    out.m[0][1] = (cs * d.k[2] - sn * d.k[3]) * d.scaleY;
    out.m[1][1] = (sn * d.k[2] + cs * d.k[3]) * d.scaleY;
    out.m[0][3] = d.translateX;
    out.m[1][3] = d.translateY;
    return out;
}

PrDMatrix4 interpolate2D(const PrDMatrix4& from, const PrDMatrix4& to, double progress) {
    Decomposed2D x = decompose2D(from);
    Decomposed2D y = decompose2D(to);
    // A rotation of more than half a turn goes the shorter way. Negating both scales and turning by half a turn keeps
    // the matrix the same.
    if ((x.scaleX < 0 && y.scaleY < 0) || (x.scaleY < 0 && y.scaleX < 0)) {
        x.scaleX = -x.scaleX;
        x.scaleY = -x.scaleY;
        x.angle += x.angle < 0 ? kPiF64 : -kPiF64;
    }
    if (absF64(x.angle - y.angle) > kPiF64) {
        if (x.angle > y.angle)
            x.angle -= 2 * kPiF64;
        else
            y.angle -= 2 * kPiF64;
    }
    Decomposed2D m{};
    m.translateX = lerpF64(x.translateX, y.translateX, progress);
    m.translateY = lerpF64(x.translateY, y.translateY, progress);
    m.scaleX = lerpF64(x.scaleX, y.scaleX, progress);
    m.scaleY = lerpF64(x.scaleY, y.scaleY, progress);
    m.angle = lerpF64(x.angle, y.angle, progress);
    for (uint32_t k = 0; k < 4; ++k)
        m.k[k] = lerpF64(x.k[k], y.k[k], progress);
    return recompose2D(m);
}

// A 3D matrix decomposed (css-transforms-2/#decomposing-a-3d-matrix).
struct Decomposed3D {
    double translate[3];
    double scale[3];
    double skew[3];                 // XY, XZ and YZ
    double perspective[4];
    double quaternion[4];
};

double dot3(const double* a, const double* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Decomposes `a`, following the specification's pseudocode, which indexes its matrix [column][row] of this file's form.
bool decompose3D(const PrDMatrix4& a, Decomposed3D* out) {
    PrDMatrix4 matrix = transposed(a);
    if (matrix.m[3][3] == 0)
        return false;
    double w = matrix.m[3][3];
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t j = 0; j < 4; ++j)
            matrix.m[i][j] /= w;
    }
    PrDMatrix4 perspectiveMatrix = matrix;
    for (uint32_t i = 0; i < 3; ++i)
        perspectiveMatrix.m[i][3] = 0;
    perspectiveMatrix.m[3][3] = 1;
    PrDMatrix4 inverse{};
    if (!prDMatrix4Invert(&perspectiveMatrix, &inverse))
        return false;
    Decomposed3D d{};
    if (matrix.m[0][3] != 0 || matrix.m[1][3] != 0 || matrix.m[2][3] != 0) {
        double rhs[4] = {matrix.m[0][3], matrix.m[1][3], matrix.m[2][3], matrix.m[3][3]};
        // The right-hand side times the transposed inverse, as a row vector.
        for (uint32_t j = 0; j < 4; ++j) {
            double sum = 0;
            for (uint32_t i = 0; i < 4; ++i)
                sum += rhs[i] * inverse.m[j][i];
            d.perspective[j] = sum;
        }
    } else {
        d.perspective[3] = 1;
    }
    for (uint32_t i = 0; i < 3; ++i)
        d.translate[i] = matrix.m[3][i];
    double row[3][3];
    for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t j = 0; j < 3; ++j)
            row[i][j] = matrix.m[i][j];
    }
    auto normalize = [](double* v, double length) {
        if (length != 0) {
            for (uint32_t k = 0; k < 3; ++k)
                v[k] /= length;
        }
    };
    auto combine = [](double* v, const double* by, double scale) {
        for (uint32_t k = 0; k < 3; ++k)
            v[k] -= by[k] * scale;
    };
    d.scale[0] = sqrtF64(dot3(row[0], row[0]));
    normalize(row[0], d.scale[0]);
    d.skew[0] = dot3(row[0], row[1]);
    combine(row[1], row[0], d.skew[0]);
    d.scale[1] = sqrtF64(dot3(row[1], row[1]));
    normalize(row[1], d.scale[1]);
    if (d.scale[1] != 0)
        d.skew[0] /= d.scale[1];
    d.skew[1] = dot3(row[0], row[2]);
    combine(row[2], row[0], d.skew[1]);
    d.skew[2] = dot3(row[1], row[2]);
    combine(row[2], row[1], d.skew[2]);
    d.scale[2] = sqrtF64(dot3(row[2], row[2]));
    normalize(row[2], d.scale[2]);
    if (d.scale[2] != 0) {
        d.skew[1] /= d.scale[2];
        d.skew[2] /= d.scale[2];
    }
    double cross[3] = {row[1][1] * row[2][2] - row[1][2] * row[2][1], row[1][2] * row[2][0] - row[1][0] * row[2][2],
                       row[1][0] * row[2][1] - row[1][1] * row[2][0]};
    if (dot3(row[0], cross) < 0) {
        for (uint32_t i = 0; i < 3; ++i) {
            d.scale[i] = -d.scale[i];
            for (uint32_t j = 0; j < 3; ++j)
                row[i][j] = -row[i][j];
        }
    }
    auto half = [](double x) { return 0.5 * sqrtF64(x > 0 ? x : 0); };
    d.quaternion[0] = half(1 + row[0][0] - row[1][1] - row[2][2]);
    d.quaternion[1] = half(1 - row[0][0] + row[1][1] - row[2][2]);
    d.quaternion[2] = half(1 - row[0][0] - row[1][1] + row[2][2]);
    d.quaternion[3] = half(1 + row[0][0] + row[1][1] + row[2][2]);
    if (row[2][1] > row[1][2])
        d.quaternion[0] = -d.quaternion[0];
    if (row[0][2] > row[2][0])
        d.quaternion[1] = -d.quaternion[1];
    if (row[1][0] > row[0][1])
        d.quaternion[2] = -d.quaternion[2];
    *out = d;
    return true;
}

// Recomposes a decomposed matrix (css-transforms-2/#recomposing-to-a-3d-matrix), in the specification's indexing.
PrDMatrix4 recompose3D(const Decomposed3D& d) {
    PrDMatrix4 matrix = prDMatrix4Identity();
    for (uint32_t i = 0; i < 4; ++i)
        matrix.m[i][3] = d.perspective[i];
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t j = 0; j < 3; ++j)
            matrix.m[3][i] += d.translate[j] * matrix.m[j][i];
    }
    double x = d.quaternion[0];
    double y = d.quaternion[1];
    double z = d.quaternion[2];
    double w = d.quaternion[3];
    PrDMatrix4 rotate = prDMatrix4Identity();
    rotate.m[0][0] = 1 - 2 * (y * y + z * z);
    rotate.m[0][1] = 2 * (x * y - z * w);
    rotate.m[0][2] = 2 * (x * z + y * w);
    rotate.m[1][0] = 2 * (x * y + z * w);
    rotate.m[1][1] = 1 - 2 * (x * x + z * z);
    rotate.m[1][2] = 2 * (y * z - x * w);
    rotate.m[2][0] = 2 * (x * z - y * w);
    rotate.m[2][1] = 2 * (y * z + x * w);
    rotate.m[2][2] = 1 - 2 * (x * x + y * y);
    // The pseudocode's multiply(matrix, m) applies m before the matrix, which in its indexing is m * matrix. Its
    // rotation matrix is written in this file's indexing, so it is transposed into the pseudocode's.
    PrDMatrix4 turned = transposed(rotate);
    matrix = prDMatrix4Concat(&matrix, &turned);
    PrDMatrix4 temp = prDMatrix4Identity();
    if (d.skew[2] != 0) {
        temp.m[2][1] = d.skew[2];
        matrix = prDMatrix4Concat(&matrix, &temp);
    }
    if (d.skew[1] != 0) {
        temp.m[2][1] = 0;
        temp.m[2][0] = d.skew[1];
        matrix = prDMatrix4Concat(&matrix, &temp);
    }
    if (d.skew[0] != 0) {
        temp.m[2][0] = 0;
        temp.m[1][0] = d.skew[0];
        matrix = prDMatrix4Concat(&matrix, &temp);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t j = 0; j < 4; ++j)
            matrix.m[i][j] *= d.scale[i];
    }
    return transposed(matrix);
}

// Interpolates two matrices decomposed (css-transforms-2/#matrix-interpolation), with css-transforms-1's 2D form if
// both are 2D. Returns false if either cannot be decomposed.
bool interpolateMatrices(const PrDMatrix4& from, const PrDMatrix4& to, double progress, PrDMatrix4* out) {
    if (is2D(from) && is2D(to)) {
        *out = interpolate2D(from, to, progress);
        return true;
    }
    Decomposed3D a{};
    Decomposed3D b{};
    if (!decompose3D(from, &a) || !decompose3D(to, &b))
        return false;
    Decomposed3D m{};
    for (uint32_t i = 0; i < 3; ++i) {
        m.translate[i] = lerpF64(a.translate[i], b.translate[i], progress);
        m.scale[i] = lerpF64(a.scale[i], b.scale[i], progress);
        m.skew[i] = lerpF64(a.skew[i], b.skew[i], progress);
    }
    for (uint32_t i = 0; i < 4; ++i)
        m.perspective[i] = lerpF64(a.perspective[i], b.perspective[i], progress);
    // Slerp (css-transforms-2/#interpolation-of-decomposed-3d-matrix-values).
    double product = 0;
    for (uint32_t i = 0; i < 4; ++i)
        product += a.quaternion[i] * b.quaternion[i];
    product = product < -1 ? -1 : product > 1 ? 1 : product;
    if (absF64(product) == 1) {
        for (uint32_t i = 0; i < 4; ++i)
            m.quaternion[i] = a.quaternion[i];
    } else {
        double theta = acosF64(product);
        double w = sinF64(progress * theta) / sqrtF64(1 - product * product);
        for (uint32_t i = 0; i < 4; ++i)
            m.quaternion[i] = a.quaternion[i] * (cosF64(progress * theta) - product * w) + b.quaternion[i] * w;
    }
    *out = recompose3D(m);
    return true;
}

// Returns the inverse of a perspective's distance, which interpolates linearly: zero for none.
double inverseDistance(float distance) {
    return distance > 0 ? 1 / double(distance < 1 ? 1 : distance) : 0;
}

// Interpolates two functions of the same kind (css-transforms-2/#interpolation-of-transform-functions).
PrLayoutTransformFunction interpolateFunction(const PrLayoutTransformFunction& a, const PrLayoutTransformFunction& b,
                                              double progress, PrVec2 extent) {
    PrLayoutTransformFunction out{};
    out.kind = a.kind;
    switch (a.kind) {
        case PrLayoutTransformKind_Rotate: {
            double x[3] = {double(a.values[0]), double(a.values[1]), double(a.values[2])};
            double y[3] = {double(b.values[0]), double(b.values[1]), double(b.values[2])};
            double lx = sqrtF64(dot3(x, x));
            double ly = sqrtF64(dot3(y, y));
            bool same = lx > 0 && ly > 0;
            for (uint32_t k = 0; k < 3 && same; ++k)
                same = absF64(x[k] / lx - y[k] / ly) < 1e-6;
            bool zero = a.values[3] == 0 || b.values[3] == 0;
            if (!same && !zero) {
                PrDMatrix4 m{};
                PrDMatrix4 from = functionMatrix(a, extent);
                PrDMatrix4 to = functionMatrix(b, extent);
                if (!interpolateMatrices(from, to, progress, &m))
                    m = progress < 0.5 ? from : to;
                return matrixFunction(m);
            }
            // The axis of the rotation that turns, or z if neither does.
            const PrLayoutTransformFunction& axis = a.values[3] != 0 ? a : b;
            if (axis.values[3] != 0) {
                out.values[0] = axis.values[0];
                out.values[1] = axis.values[1];
                out.values[2] = axis.values[2];
            } else {
                out.values[2] = 1;
            }
            out.values[3] = float(lerpF64(double(a.values[3]), double(b.values[3]), progress));
            return out;
        }
        case PrLayoutTransformKind_Perspective: {
            double inverse = lerpF64(inverseDistance(a.values[0]), inverseDistance(b.values[0]), progress);
            out.values[0] = inverse > 0 ? float(1 / inverse) : 0;
            return out;
        }
        case PrLayoutTransformKind_Matrix: {
            PrDMatrix4 m{};
            PrDMatrix4 from = functionMatrix(a, extent);
            PrDMatrix4 to = functionMatrix(b, extent);
            if (!interpolateMatrices(from, to, progress, &m))
                m = progress < 0.5 ? from : to;
            return matrixFunction(m);
        }
        default:
            for (uint32_t k = 0; k < 16; ++k)
                out.values[k] = float(lerpF64(double(a.values[k]), double(b.values[k]), progress));
            return out;
    }
}

PrMatrix4 matrix4Of(const PrDMatrix4& a) {
    PrMatrix4 out{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column)
            out.m[row][column] = float(a.m[row][column]);
    }
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutEasing prLayoutEasingLinear(void) {
    PrLayoutEasing easing{};
    easing.kind = PrLayoutEasingKind_Linear;
    easing.steps = 1;
    return easing;
}

double prLayoutEase(const PrLayoutEasing* easing, double input, bool before) {
    switch (easing->kind) {
        case PrLayoutEasingKind_CubicBezier:
            return bezierEase(*easing, unit(input));
        case PrLayoutEasingKind_Steps:
            return easing->steps != 0 ? stepEase(*easing, input, before) : input;
        default:
            return input;
    }
}

PrLayoutTiming prLayoutTimingDefault(void) {
    PrLayoutTiming timing{};
    timing.iterations = 1;
    timing.direction = PrLayoutPlaybackDirection_Normal;
    timing.fill = PrLayoutFill_None;
    return timing;
}

double prLayoutTimingProgress(const PrLayoutTiming* timing, double time, bool* finished) {
    const PrLayoutTiming& t = *timing;
    double local = (t.paused ? t.hold : time - t.start) - t.delay;
    double duration = t.duration > 0 ? t.duration : 0;
    double active = duration * t.iterations;
    bool backwards = t.fill == PrLayoutFill_Backwards || t.fill == PrLayoutFill_Both;
    bool forwards = t.fill == PrLayoutFill_Forwards || t.fill == PrLayoutFill_Both;
    *finished = false;
    double overall = 0;
    if (local < 0) {
        if (!backwards)
            return -1;
    } else if (local >= active) {
        *finished = true;
        if (!forwards)
            return -1;
        overall = t.iterations;
    } else {
        overall = duration > 0 ? local / duration : t.iterations;
    }
    double iteration = floorF64(overall);
    double progress = overall - iteration;
    // The end of a whole number of iterations is the end of the last iteration, not the start of the next one.
    if (progress == 0 && overall != 0 && local >= active) {
        progress = 1;
        iteration -= 1;
    }
    bool odd = fmodF64(iteration, 2) != 0;
    bool reverse = t.direction == PrLayoutPlaybackDirection_Reverse
                   || (t.direction == PrLayoutPlaybackDirection_Alternate && odd)
                   || (t.direction == PrLayoutPlaybackDirection_AlternateReverse && !odd);
    return reverse ? 1 - progress : progress;
}

PrMatrix4 prLayoutTransformListMatrix(const PrLayoutTransformList* list, PrVec2 extent) {
    return matrix4Of(listMatrix(*list, 0, extent));
}

PrLayoutTransform prLayoutTransformListTransform(const PrLayoutTransformList* list) {
    // The product so far, at zero size, and the vectors that the width and the height add to its last column.
    PrDMatrix4 total = prDMatrix4Identity();
    double perWidth[4] = {};
    double perHeight[4] = {};
    for (uint32_t i = 0; i < list->count; ++i) {
        const PrLayoutTransformFunction& f = list->functions[i];
        if (f.kind == PrLayoutTransformKind_Translate) {
            for (uint32_t row = 0; row < 4; ++row) {
                perWidth[row] += total.m[row][0] * double(f.values[3]);
                perHeight[row] += total.m[row][1] * double(f.values[4]);
            }
        }
        PrDMatrix4 next = functionMatrix(f, PrVec2{0, 0});
        // A function with perspective would move the size into the other columns, which the form cannot hold.
        if (next.m[3][0] != 0 || next.m[3][1] != 0 || next.m[3][2] != 0 || next.m[3][3] != 1) {
            for (uint32_t row = 0; row < 4; ++row)
                perWidth[row] = perHeight[row] = 0;
        }
        total = prDMatrix4Concat(&next, &total);
    }
    PrLayoutTransform out{.matrix = matrix4Of(total)};
    for (uint32_t row = 0; row < 4; ++row) {
        out.perWidth[row] = float(perWidth[row]);
        out.perHeight[row] = float(perHeight[row]);
    }
    return out;
}

PrLayoutTransformList prLayoutTransformListInterpolate(const PrLayoutTransformList* from,
                                                       const PrLayoutTransformList* to, double progress,
                                                       PrVec2 extent) {
    PrLayoutTransformList a = *from;
    PrLayoutTransformList b = *to;
    // The shorter list is extended with the identity functions of the longer list's kinds.
    for (; a.count < b.count; ++a.count)
        a.functions[a.count] = identityLike(b.functions[a.count]);
    for (; b.count < a.count; ++b.count)
        b.functions[b.count] = identityLike(a.functions[b.count]);
    PrLayoutTransformList out{};
    for (uint32_t i = 0; i < a.count; ++i) {
        if (a.functions[i].kind == b.functions[i].kind) {
            out.functions[out.count++] = interpolateFunction(a.functions[i], b.functions[i], progress, extent);
            continue;
        }
        PrDMatrix4 x = listMatrix(a, i, extent);
        PrDMatrix4 y = listMatrix(b, i, extent);
        PrDMatrix4 m{};
        if (!interpolateMatrices(x, y, progress, &m))
            m = progress < 0.5 ? x : y;
        out.functions[out.count++] = matrixFunction(m);
        break;
    }
    return out;
}

bool prLayoutTransformListInterpolates(const PrLayoutTransformList* from, const PrLayoutTransformList* to,
                                       PrVec2 extent) {
    PrLayoutTransformList a = *from;
    PrLayoutTransformList b = *to;
    for (; a.count < b.count; ++a.count)
        a.functions[a.count] = identityLike(b.functions[a.count]);
    for (; b.count < a.count; ++b.count)
        b.functions[b.count] = identityLike(a.functions[b.count]);
    PrDMatrix4 m{};
    for (uint32_t i = 0; i < a.count; ++i) {
        const PrLayoutTransformFunction& x = a.functions[i];
        const PrLayoutTransformFunction& y = b.functions[i];
        if (x.kind != y.kind)
            return interpolateMatrices(listMatrix(a, i, extent), listMatrix(b, i, extent), 0.5, &m);
        // A matrix, and a rotation that turns about another axis, interpolate decomposed. Testing a rotation's pair is
        // harmless, because a rotation can always be decomposed.
        if ((x.kind == PrLayoutTransformKind_Matrix || x.kind == PrLayoutTransformKind_Rotate)
            && !interpolateMatrices(functionMatrix(x, extent), functionMatrix(y, extent), 0.5, &m))
            return false;
    }
    return true;
}

bool prLayoutTrackSample(const PrLayoutTrack* track, double time, PrVec2 extent, float* opacity,
                         PrMatrix4* transform) {
    bool finished = false;
    double progress = prLayoutTimingProgress(&track->timing, time, &finished);
    if (progress < 0 || track->keyframeCount == 0)
        return false;
    const PrLayoutKeyframe* frames = track->keyframes;
    uint32_t at = 0;
    while (at + 1 < track->keyframeCount && frames[at + 1].offset <= progress)
        ++at;
    const PrLayoutKeyframe& from = frames[at];
    const PrLayoutKeyframe& to = at + 1 < track->keyframeCount ? frames[at + 1] : frames[at];
    double span = to.offset - from.offset;
    double local = span > 0 ? prLayoutEase(&from.easing, (progress - from.offset) / span, false) : 1;
    if (track->effect == PrLayoutEffect_Opacity) {
        double value = lerpF64(double(from.opacity), double(to.opacity), local);
        *opacity = float(value < 0 ? 0 : value > 1 ? 1 : value);
    } else {
        PrLayoutTransformList between = prLayoutTransformListInterpolate(&from.transform, &to.transform, local, extent);
        *transform = prLayoutTransformListMatrix(&between, extent);
    }
    return true;
}
