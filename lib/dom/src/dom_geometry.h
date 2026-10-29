// Geometry interfaces
//
// geometry-1 - https://drafts.csswg.org/geometry/

#pragma once

#include "pr/dom_geometry.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The indices of a, b, c, d, e and f in a matrix's m.
constexpr uint32_t kDomMatrix2DIndices[] = {0, 1, 4, 5, 12, 13};

// Returns the rotation by `angle` radians about the axis (x, y, z). The rotation is the identity for an axis of zero
// length (css-transforms-2/#funcdef-rotate3d).
PrDomMatrix domMatrixRotation(double x, double y, double z, double angle);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
