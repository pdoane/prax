// Geometry interfaces
//
// geometry-1 - https://drafts.csswg.org/geometry/

#include "pr/dom_geometry.h"
#include "dom/src/dom_geometry.h"

#include "pr/float.h"
#include "pr/mem.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

bool is2DIndex(uint32_t index) {
    for (uint32_t index2D : kDomMatrix2DIndices) {
        if (index == index2D)
            return true;
    }
    return false;
}

// Writes `left` times `right` into `self`. Element (row, column) is m[column * 4 + row].
void multiply(PrDomMatrix* self, const PrDomMatrix* left, const PrDomMatrix* right) {
    double product[16] = {};
    for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
            for (uint32_t i = 0; i < 4; ++i)
                product[column * 4 + row] += left->m[i * 4 + row] * right->m[column * 4 + i];
        }
    }
    memCopy(self->m, product, sizeof(product));
    self->is2D = left->is2D && right->is2D;
}

// geometry-1/#nan-safe-minimum and geometry-1/#nan-safe-maximum: return NaN if either argument is NaN.
double nanSafeMin(double a, double b) {
    return isNanF64(a) || isNanF64(b) ? kNanF64 : a < b ? a : b;
}

double nanSafeMax(double a, double b) {
    return isNanF64(a) || isNanF64(b) ? kNanF64 : a > b ? a : b;
}

} // namespace

PrDomMatrix domMatrixRotation(double x, double y, double z, double angle) {
    PrDomMatrix result = prDomMatrixIdentity();
    double length = hypotF64(hypotF64(x, y), z);
    if (length == 0)
        return result;
    x /= length;
    y /= length;
    z /= length;
    double c = cosF64(angle);
    double s = sinF64(angle);
    // For a rotation about an axis of the coordinate system, the elements along that axis stay exactly the same as in
    // the identity.
    if (x == 0 && y == 0) {
        result.m[0] = result.m[5] = c;
        result.m[1] = s * z;
        result.m[4] = -s * z;
        return result;
    }
    if (x == 0 && z == 0) {
        result.m[0] = result.m[10] = c;
        result.m[2] = -s * y;
        result.m[8] = s * y;
        result.is2D = false;
        return result;
    }
    if (y == 0 && z == 0) {
        result.m[5] = result.m[10] = c;
        result.m[6] = s * x;
        result.m[9] = -s * x;
        result.is2D = false;
        return result;
    }
    double t = 1 - c;
    result.m[0] = t * x * x + c;
    result.m[1] = t * x * y + s * z;
    result.m[2] = t * x * z - s * y;
    result.m[4] = t * x * y - s * z;
    result.m[5] = t * y * y + c;
    result.m[6] = t * y * z + s * x;
    result.m[8] = t * x * z + s * y;
    result.m[9] = t * y * z - s * x;
    result.m[10] = t * z * z + c;
    result.is2D = false;
    return result;
}

} // namespace pr_impl

using namespace pr_impl;

PrDomMatrix prDomMatrixIdentity(void) {
    PrDomMatrix matrix = {};
    matrix.m[0] = matrix.m[5] = matrix.m[10] = matrix.m[15] = 1;
    matrix.is2D = true;
    return matrix;
}

PrStatus prDomMatrixFromValues(const double* PR_NULLABLE values, uint32_t count, PrDomMatrix* out) {
    if (count != 6 && count != 16)
        return PrStatus_InvalidData;
    *out = prDomMatrixIdentity();
    if (count == 16) {
        memCopy(out->m, values, sizeof(out->m));
        out->is2D = false;
    } else {
        for (uint32_t i = 0; i < 6; ++i)
            out->m[kDomMatrix2DIndices[i]] = values[i];
    }
    return PrStatus_Ok;
}

void prDomMatrixMultiply(PrDomMatrix* self, const PrDomMatrix* other) {
    PrDomMatrix left = *self;
    multiply(self, &left, other);
}

void prDomMatrixPreMultiply(PrDomMatrix* self, const PrDomMatrix* other) {
    PrDomMatrix right = *self;
    multiply(self, other, &right);
}

void prDomMatrixTranslate(PrDomMatrix* self, double tx, double ty, double tz) {
    PrDomMatrix operation = prDomMatrixIdentity();
    operation.m[12] = tx;
    operation.m[13] = ty;
    operation.m[14] = tz;
    operation.is2D = tz == 0;
    prDomMatrixMultiply(self, &operation);
}

void prDomMatrixScale(PrDomMatrix* self, double sx, double sy, double sz, double ox, double oy, double oz) {
    PrDomMatrix operation = prDomMatrixIdentity();
    operation.m[0] = sx;
    operation.m[5] = sy;
    operation.m[10] = sz;
    operation.m[12] = ox * (1 - sx);
    operation.m[13] = oy * (1 - sy);
    operation.m[14] = oz * (1 - sz);
    operation.is2D = sz == 1 && oz == 0;
    prDomMatrixMultiply(self, &operation);
}

void prDomMatrixRotate(PrDomMatrix* self, double x, double y, double z) {
    bool is2D = self->is2D && x == 0 && y == 0;
    PrDomMatrix about = domMatrixRotation(0, 0, 1, degreesToRadiansF64(z));
    prDomMatrixMultiply(self, &about);
    about = domMatrixRotation(0, 1, 0, degreesToRadiansF64(y));
    prDomMatrixMultiply(self, &about);
    about = domMatrixRotation(1, 0, 0, degreesToRadiansF64(x));
    prDomMatrixMultiply(self, &about);
    self->is2D = is2D;
}

void prDomMatrixRotateFromVector(PrDomMatrix* self, double x, double y) {
    PrDomMatrix about = domMatrixRotation(0, 0, 1, x == 0 && y == 0 ? 0 : atan2F64(y, x));
    prDomMatrixMultiply(self, &about);
}

void prDomMatrixRotateAxisAngle(PrDomMatrix* self, double x, double y, double z, double angle) {
    PrDomMatrix about = domMatrixRotation(x, y, z, degreesToRadiansF64(angle));
    about.is2D = x == 0 && y == 0;
    prDomMatrixMultiply(self, &about);
}

void prDomMatrixSkewX(PrDomMatrix* self, double degrees) {
    PrDomMatrix operation = prDomMatrixIdentity();
    operation.m[4] = tanF64(degreesToRadiansF64(degrees));
    prDomMatrixMultiply(self, &operation);
}

void prDomMatrixSkewY(PrDomMatrix* self, double degrees) {
    PrDomMatrix operation = prDomMatrixIdentity();
    operation.m[1] = tanF64(degreesToRadiansF64(degrees));
    prDomMatrixMultiply(self, &operation);
}

bool prDomMatrixInvert(PrDomMatrix* self) {
    const double* m = self->m;
    double inverse[16];
    inverse[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
        m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inverse[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
        m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inverse[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
        m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inverse[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
        m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inverse[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
        m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inverse[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
        m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inverse[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
        m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inverse[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
        m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inverse[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
        m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inverse[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
        m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inverse[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
        m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inverse[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
        m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inverse[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
        m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inverse[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
        m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inverse[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
        m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inverse[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
        m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double determinant = m[0] * inverse[0] + m[1] * inverse[4] + m[2] * inverse[8] + m[3] * inverse[12];
    if (determinant == 0 || !isFiniteF64(determinant)) {
        for (double& element : self->m)
            element = kNanF64;
        self->is2D = false;
        return false;
    }
    for (uint32_t i = 0; i < 16; ++i)
        self->m[i] = inverse[i] / determinant;
    if (self->is2D) {
        // The inverse of a 2D matrix is 2D, and its 3D elements are exactly those of the identity matrix.
        PrDomMatrix identity = prDomMatrixIdentity();
        for (uint32_t i = 0; i < 16; ++i) {
            if (!is2DIndex(i))
                self->m[i] = identity.m[i];
        }
    }
    return true;
}

void prDomMatrixSetElement(PrDomMatrix* self, uint32_t index, double value) {
    self->m[index] = value;
    if (!is2DIndex(index) && value != prDomMatrixIdentity().m[index])
        self->is2D = false;
}

bool prDomMatrixIsIdentity(const PrDomMatrix* self) {
    PrDomMatrix identity = prDomMatrixIdentity();
    for (uint32_t i = 0; i < 16; ++i) {
        if (self->m[i] != identity.m[i])
            return false;
    }
    return true;
}

PrDomPoint prDomPointMatrixTransform(const PrDomPoint* self, const PrDomMatrix* matrix) {
    const double* m = matrix->m;
    PrDomPoint result;
    result.x = m[0] * self->x + m[4] * self->y + m[8] * self->z + m[12] * self->w;
    result.y = m[1] * self->x + m[5] * self->y + m[9] * self->z + m[13] * self->w;
    result.z = m[2] * self->x + m[6] * self->y + m[10] * self->z + m[14] * self->w;
    result.w = m[3] * self->x + m[7] * self->y + m[11] * self->z + m[15] * self->w;
    return result;
}

double prDomRectTop(const PrDomRect* self) {
    return nanSafeMin(self->y, self->y + self->height);
}

double prDomRectRight(const PrDomRect* self) {
    return nanSafeMax(self->x, self->x + self->width);
}

double prDomRectBottom(const PrDomRect* self) {
    return nanSafeMax(self->y, self->y + self->height);
}

double prDomRectLeft(const PrDomRect* self) {
    return nanSafeMin(self->x, self->x + self->width);
}

PrDomQuad prDomQuadFromRect(const PrDomRect* rect) {
    double right = rect->x + rect->width;
    double bottom = rect->y + rect->height;
    return {{{rect->x, rect->y, 0, 1}, {right, rect->y, 0, 1}, {right, bottom, 0, 1}, {rect->x, bottom, 0, 1}}};
}

PrDomRect prDomQuadGetBounds(const PrDomQuad* self) {
    double left = self->p[0].x;
    double top = self->p[0].y;
    double right = left;
    double bottom = top;
    for (uint32_t i = 1; i < 4; ++i) {
        left = nanSafeMin(left, self->p[i].x);
        top = nanSafeMin(top, self->p[i].y);
        right = nanSafeMax(right, self->p[i].x);
        bottom = nanSafeMax(bottom, self->p[i].y);
    }
    return {left, top, right - left, bottom - top};
}
