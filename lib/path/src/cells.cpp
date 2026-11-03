// Area coverage accumulated in cells along rows of pixels.

#include "path/src/cells.h"

#include "pr/float.h"

namespace pr_impl {

float pathCoverageFromWinding(float winding, PrPathFillRule rule) {
    float magnitude = prF32Abs(winding);
    if (rule == PrPathFillRule_EvenOdd) {
        magnitude -= 2 * prF32Floor(magnitude / 2);
        return magnitude > 1 ? 2 - magnitude : magnitude;
    }
    return prF32Min(magnitude, 1);
}

void pathAccumulate(float* cells, size_t stride, uint32_t width, uint32_t height, PathCellEdge e) {
    float y0 = prF32Max(e.y0, 0);
    float y1 = prF32Min(e.y1, float(height));
    if (y0 >= y1)
        return;
    float slope = (e.x1 - e.x0) / (e.y1 - e.y0);
    float x = prF32Min(prF32Max(e.x0 + (y0 - e.y0) * slope, 0), float(width));
    for (uint32_t y = uint32_t(y0); float(y) < y1; ++y) {
        float* row = cells + size_t(y) * stride;
        float dy = prF32Min(float(y + 1), y1) - prF32Max(float(y), y0);
        float xNext = prF32Min(prF32Max(x + slope * dy, 0), float(width));
        float d = dy * e.direction;
        float xLow = prF32Min(x, xNext);
        float xHigh = prF32Max(x, xNext);
        float lowFloor = prF32Floor(xLow);
        uint32_t lowCell = uint32_t(lowFloor);
        uint32_t highCell = uint32_t(prF32Ceil(xHigh));
        if (highCell <= lowCell + 1) {
            float middle = (x + xNext) / 2 - lowFloor;
            row[lowCell] += d - d * middle;
            row[lowCell + 1] += d * middle;
        } else {
            float inverse = 1 / (xHigh - xLow);
            float lowFraction = xLow - lowFloor;
            float first = inverse * (1 - lowFraction) * (1 - lowFraction) / 2;
            float highFraction = xHigh - float(highCell) + 1;
            float last = inverse * highFraction * highFraction / 2;
            row[lowCell] += d * first;
            if (highCell == lowCell + 2) {
                row[lowCell + 1] += d * (1 - first - last);
            } else {
                float second = inverse * (1.5f - lowFraction);
                row[lowCell + 1] += d * (second - first);
                for (uint32_t cell = lowCell + 2; cell < highCell - 1; ++cell)
                    row[cell] += d * inverse;
                float before = second + float(highCell - lowCell - 3) * inverse;
                row[highCell - 1] += d * (1 - before - last);
            }
            row[highCell] += d * last;
        }
        x = xNext;
    }
}

} // namespace pr_impl
