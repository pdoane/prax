// Geometry interfaces
//
// geometry-1 - https://drafts.csswg.org/geometry/

#include "pr/dom_geometry.h"
#include "pr/float.h"

#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

std::string text(const PrDomMatrix& matrix) {
    PrString out = {};
    REQUIRE(prCssMatrixToString(&matrix, &out) == PrStatus_Ok);
    return take(out);
}

PrDomMatrix parsed(const char* transform) {
    PrDomMatrix matrix;
    REQUIRE(prCssMatrixFromString(transform, &matrix) == PrStatus_Ok);
    return matrix;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a matrix is 2D from six values and 3D from sixteen") {
    const double values[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    PrDomMatrix matrix;
    REQUIRE(prDomMatrixFromValues(values, 6, &matrix) == PrStatus_Ok);
    CHECK(matrix.is2D);
    CHECK(text(matrix) == "matrix(1, 2, 3, 4, 5, 6)");
    REQUIRE(prDomMatrixFromValues(values, 16, &matrix) == PrStatus_Ok);
    CHECK_FALSE(matrix.is2D);
    CHECK(text(matrix) == "matrix3d(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)");
    CHECK(prDomMatrixFromValues(values, 5, &matrix) == PrStatus_InvalidData);
}

TEST_CASE("a transform list parses to its matrix, and a relative length does not") {
    CHECK(text(parsed("")) == "matrix(1, 0, 0, 1, 0, 0)");
    CHECK(text(parsed("translate(10px, 20px) scale(2)")) == "matrix(2, 0, 0, 2, 10, 20)");
    CHECK_FALSE(parsed("translateZ(1px)").is2D);
    PrDomMatrix matrix = prDomMatrixIdentity();
    CHECK(prCssMatrixFromString("translate(1em)", &matrix) == PrStatus_Syntax);
    CHECK(prCssMatrixFromString("nonsense", &matrix) == PrStatus_Syntax);
    CHECK(prDomMatrixIsIdentity(&matrix));
}

TEST_CASE("operations set is2D as the DOMMatrix methods do") {
    PrDomMatrix matrix = prDomMatrixIdentity();
    prDomMatrixTranslate(&matrix, 1, 2, 0);
    CHECK(matrix.is2D);
    prDomMatrixRotate(&matrix, 0, 0, 90);
    CHECK(matrix.is2D);
    prDomMatrixScale(&matrix, 2, 2, 2, 0, 0, 0);
    CHECK_FALSE(matrix.is2D);

    matrix = prDomMatrixIdentity();
    prDomMatrixSetElement(&matrix, 0, 5);
    CHECK(matrix.is2D);
    prDomMatrixSetElement(&matrix, 10, 1);
    CHECK(matrix.is2D);
    prDomMatrixSetElement(&matrix, 2, 1);
    CHECK_FALSE(matrix.is2D);
}

TEST_CASE("an inverse keeps a 2D matrix 2D, and a singular matrix becomes NaN") {
    PrDomMatrix matrix = parsed("translate(10px, 20px) scale(2)");
    REQUIRE(prDomMatrixInvert(&matrix));
    CHECK(matrix.is2D);
    CHECK(text(matrix) == "matrix(0.5, 0, 0, 0.5, -5, -10)");

    matrix = parsed("scale(0)");
    CHECK_FALSE(prDomMatrixInvert(&matrix));
    CHECK_FALSE(matrix.is2D);
    PrString out = {};
    CHECK(prCssMatrixToString(&matrix, &out) == PrStatus_InvalidState);
}

TEST_CASE("multiplication is post- or pre-multiplication") {
    PrDomMatrix translate = parsed("translate(10px)");
    PrDomMatrix scale = parsed("scale(2)");
    PrDomMatrix post = translate;
    prDomMatrixMultiply(&post, &scale);
    CHECK(text(post) == "matrix(2, 0, 0, 2, 10, 0)");
    PrDomMatrix pre = translate;
    prDomMatrixPreMultiply(&pre, &scale);
    CHECK(text(pre) == "matrix(2, 0, 0, 2, 20, 0)");
}

TEST_CASE("a rotation about an axis keeps the other axes exactly") {
    PrDomMatrix matrix = prDomMatrixIdentity();
    prDomMatrixRotate(&matrix, 0, 0, 38);
    CHECK(matrix.m[10] == 1);
    CHECK(matrix.m[15] == 1);
    CHECK(matrix.is2D);
    matrix = parsed("rotateX(38deg)");
    CHECK(matrix.m[0] == 1);
    CHECK_FALSE(matrix.is2D);
}

TEST_CASE("a point transforms in homogeneous coordinates") {
    PrDomMatrix matrix = parsed("translate(10px, 20px) scale(2)");
    PrDomPoint point = {1, 1, 0, 1};
    PrDomPoint moved = prDomPointMatrixTransform(&point, &matrix);
    CHECK(moved.x == 12);
    CHECK(moved.y == 22);
    CHECK(moved.w == 1);
    point.w = 0;
    moved = prDomPointMatrixTransform(&point, &matrix);
    CHECK(moved.x == 2);
    CHECK(moved.y == 2);
}

TEST_CASE("a rectangle's edges hold whichever sign its size has, and NaN spreads") {
    PrDomRect rect = {10, 20, -5, 30};
    CHECK(prDomRectLeft(&rect) == 5);
    CHECK(prDomRectRight(&rect) == 10);
    CHECK(prDomRectTop(&rect) == 20);
    CHECK(prDomRectBottom(&rect) == 50);
    rect.x = kPrF64Nan;
    CHECK(prF64IsNan(prDomRectLeft(&rect)));
    CHECK(prF64IsNan(prDomRectRight(&rect)));
    CHECK(prDomRectTop(&rect) == 20);
}

TEST_CASE("a quad's bounds hold its corners") {
    PrDomRect rect = {1, 2, 3, 4};
    PrDomQuad quad = prDomQuadFromRect(&rect);
    CHECK(quad.p[2].x == 4);
    CHECK(quad.p[2].y == 6);
    quad.p[1].x = -10;
    PrDomRect bounds = prDomQuadGetBounds(&quad);
    CHECK(bounds.x == -10);
    CHECK(bounds.width == 14);
    CHECK(bounds.height == 4);
    quad.p[3].y = kPrF64Nan;
    bounds = prDomQuadGetBounds(&quad);
    CHECK(prF64IsNan(bounds.y));
    CHECK(bounds.x == -10);
}
