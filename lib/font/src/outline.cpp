// Glyph outlines as fonts store them.

#include "font/src/outline.h"

#include "pr/float.h"

namespace pr_impl {

namespace {

// The winding of an outline's contours: counterclockwise with y up, as TrueType draws its outer contours, or clockwise,
// as PostScript does, or none for an outline without area.
enum {
    Winding_None,
    Winding_TrueType,
    Winding_PostScript,
};
typedef uint8_t Winding;

// Returns the winding of the polygon that the outline's points span, by the sign of its area under the nonzero rule.
Winding windingOf(const FontGlyphOutline& outline) {
    PrRect box = fontOutlineControlBox(outline);
    if (box.left == box.right || box.top == box.bottom)
        return Winding_None;
    double area = 0;
    uint32_t first = 0;
    for (uint32_t c = 0; c < outline.ends.count; ++c) {
        uint32_t last = outline.ends.data[c] - 1;
        PrVec2 previous = outline.points.data[last];
        for (uint32_t i = first; i <= last; ++i) {
            PrVec2 point = outline.points.data[i];
            area += double(point.y - previous.y) * double(point.x + previous.x);
            previous = point;
        }
        first = last + 1;
    }
    return area > 0 ? Winding_PostScript : area < 0 ? Winding_TrueType : Winding_None;
}

// Returns `v` scaled to length 1, and its length in `length`. A zero vector stays zero.
PrVec2 unit(PrVec2 v, float* length) {
    *length = sqrtF32(v.x * v.x + v.y * v.y);
    return *length != 0 ? PrVec2{v.x / *length, v.y / *length} : PrVec2{0, 0};
}

PrVec2 midpoint(PrVec2 a, PrVec2 b) {
    return PrVec2{(a.x + b.x) / 2, (a.y + b.y) / 2};
}

} // namespace

void fontOutlineClear(FontGlyphOutline& outline) {
    outline.points.clear();
    outline.kinds.clear();
    outline.ends.clear();
}

void fontOutlineReset(FontGlyphOutline& outline) {
    outline.points.reset();
    outline.kinds.reset();
    outline.ends.reset();
}

void fontOutlinePush(FontGlyphOutline& outline, PrVec2 point, OutlinePoint kind) {
    outline.points.push(point);
    outline.kinds.push(kind);
}

void fontOutlineEndContour(FontGlyphOutline& outline) {
    uint32_t start = outline.ends.count != 0 ? outline.ends.last() : 0;
    if (outline.points.count > start)
        outline.ends.push(outline.points.count);
}

PrRect fontOutlineControlBox(const FontGlyphOutline& outline) {
    if (outline.points.count == 0)
        return PrRect{0, 0, 0, 0};
    PrRect box{outline.points.data[0].x, outline.points.data[0].y, outline.points.data[0].x, outline.points.data[0].y};
    for (uint32_t i = 1; i < outline.points.count; ++i) {
        PrVec2 p = outline.points.data[i];
        box.left = minF32(box.left, p.x);
        box.top = minF32(box.top, p.y);
        box.right = maxF32(box.right, p.x);
        box.bottom = maxF32(box.bottom, p.y);
    }
    return box;
}

void fontOutlineTransform(FontGlyphOutline& outline, uint32_t first, PrAffine2 transform) {
    for (uint32_t i = first; i < outline.points.count; ++i)
        outline.points.data[i] = prAffine2MapPoint(transform, outline.points.data[i]);
}

void fontOutlineEmbolden(FontGlyphOutline& outline, float x, float y) {
    float xStrength = x / 2;
    float yStrength = y / 2;
    if (xStrength == 0 && yStrength == 0)
        return;
    Winding winding = windingOf(outline);
    if (winding == Winding_None)
        return;
    PrVec2* points = outline.points.data;
    uint32_t first = 0;
    for (uint32_t c = 0; c < outline.ends.count; ++c) {
        int32_t last = int32_t(outline.ends.data[c]) - 1;
        int32_t start = int32_t(first);
        first = uint32_t(last) + 1;
        PrVec2 in{0, 0};
        PrVec2 anchor{0, 0};
        float lengthIn = 0;
        float lengthAnchor = 0;
        // j cycles through the points, i advances only as points move, and k marks the first point moved.
        int32_t i = last;
        int32_t k = -1;
        for (int32_t j = start; j != i && i != k; j = j < last ? j + 1 : start) {
            PrVec2 out;
            float lengthOut = 0;
            if (j != k) {
                out = unit(PrVec2{points[j].x - points[i].x, points[j].y - points[i].y}, &lengthOut);
                if (lengthOut == 0)
                    continue;
            } else {
                out = anchor;
                lengthOut = lengthAnchor;
            }
            if (lengthIn != 0) {
                if (k < 0) {
                    k = i;
                    anchor = in;
                    lengthAnchor = lengthIn;
                }
                float d = in.x * out.x + in.y * out.y;
                PrVec2 shift{0, 0};
                // A point shifts only where the contour turns by less than about 160 degrees.
                if (d > -0.9375f) {
                    d += 1;
                    shift = PrVec2{in.y + out.y, in.x + out.x};
                    if (winding == Winding_TrueType)
                        shift.x = -shift.x;
                    else
                        shift.y = -shift.y;
                    // The shift is limited where a segment collapses.
                    float q = out.x * in.y - out.y * in.x;
                    if (winding == Winding_TrueType)
                        q = -q;
                    float l = minF32(lengthIn, lengthOut);
                    shift.x = xStrength * q <= l * d ? shift.x * xStrength / d : shift.x * l / q;
                    shift.y = yStrength * q <= l * d ? shift.y * yStrength / d : shift.y * l / q;
                }
                for (; i != j; i = i < last ? i + 1 : start) {
                    points[i].x += xStrength + shift.x;
                    points[i].y += yStrength + shift.y;
                }
            } else {
                i = j;
            }
            in = out;
            lengthIn = lengthOut;
        }
    }
}

void fontOutlineAppendPath(const FontGlyphOutline& outline, float scale, PrPath* path) {
    auto at = [&](PrVec2 p) { return PrVec2{p.x * scale, -p.y * scale}; };
    const PrVec2* points = outline.points.data;
    const OutlinePoint* kinds = outline.kinds.data;
    int32_t first = 0;
    for (uint32_t c = 0; c < outline.ends.count; ++c) {
        int32_t last = int32_t(outline.ends.data[c]) - 1;
        int32_t limit = last;
        PrVec2 start = points[first];
        int32_t point = first;
        // A contour that starts with a control point starts at its last point if that is on the curve, and otherwise
        // halfway between the two.
        if (kinds[first] == OutlinePoint_Quadratic) {
            if (kinds[last] == OutlinePoint_On) {
                start = points[last];
                --limit;
            } else {
                start = midpoint(points[first], points[last]);
            }
            --point;
        }
        prPathMoveTo(path, at(start));
        bool closed = false;
        while (point < limit && !closed) {
            ++point;
            if (kinds[point] == OutlinePoint_On) {
                prPathLineTo(path, at(points[point]));
                continue;
            }
            if (kinds[point] == OutlinePoint_Quadratic) {
                PrVec2 control = points[point];
                for (;;) {
                    if (point >= limit) {
                        prPathQuadTo(path, at(control), at(start));
                        closed = true;
                        break;
                    }
                    ++point;
                    if (kinds[point] == OutlinePoint_On) {
                        prPathQuadTo(path, at(control), at(points[point]));
                        break;
                    }
                    if (kinds[point] != OutlinePoint_Quadratic) {
                        closed = true;
                        break;
                    }
                    PrVec2 middle = midpoint(control, points[point]);
                    prPathQuadTo(path, at(control), at(middle));
                    control = points[point];
                }
                continue;
            }
            // A cubic segment: two control points, then a point on the curve or the start.
            if (point + 1 > limit || kinds[point + 1] != OutlinePoint_Cubic) {
                closed = true;
                break;
            }
            PrVec2 control1 = points[point];
            PrVec2 control2 = points[point + 1];
            point += 2;
            if (point <= limit) {
                prPathCubicTo(path, at(control1), at(control2), at(points[point]));
                continue;
            }
            prPathCubicTo(path, at(control1), at(control2), at(start));
            closed = true;
        }
        if (!closed)
            prPathLineTo(path, at(start));
        prPathClose(path);
        first = last + 1;
    }
}

} // namespace pr_impl
