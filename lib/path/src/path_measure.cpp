// Measuring paths: the arc length of their contours, taken end to end, and the nearest point on them.
//
// Each point of a contour records its arc length. A grid over the bounds of the points lists the segments that meet
// each cell, so a nearest-point query visits only the cells within its reach.

#include "pr/path_measure.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/raw_array.h"
#include "pr/search.h"
#include "pr/vec.h"

#include "path/src/path_polyline.h"

namespace pr_impl {

namespace {

// Points closer than this are treated as one point.
constexpr float kCoincident = 1e-6f;

// The maximum number of cells along each axis of the grid.
constexpr uint32_t kMaxCells = 256;

struct Contour {
    uint32_t first;         // index of the first point
    uint32_t count;         // number of points
    bool closed;
    float start;            // distance along the path at which the contour starts
    float length;
};

struct Segment {
    uint32_t from;          // index of the start point
    uint32_t to;            // index of the end point
    uint32_t contour;
};

struct PathMeasure {
    PrPathPolyline flat;                // what a path flattens to before it is measured
    float length;
    RawArray<PrVec2> points;
    RawArray<float> along;             // arc length of each point from the start of its contour
    RawArray<Contour> contours;
    RawArray<Segment> segments;
    PrRect bounds;
    uint32_t columns;
    uint32_t rows;
    float cellWidth;
    float cellHeight;

    // For each cell, the start of its segments in `cellSegments`, plus one entry past the last cell.
    RawArray<uint32_t> cellStart;
    RawArray<uint32_t> cellSegments;
};

float distanceBetween(PrVec2 a, PrVec2 b) {
    return prF32Sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

void addPoint(PathMeasure& m, PrVec2 p) {
    Contour& c = m.contours.last();
    if (c.count != 0 && distanceBetween(m.points.last(), p) < kCoincident)
        return;
    m.points.push(p);
    ++c.count;
}

// Ends the contour being built, dropping it if it has no segment.
void endContour(PathMeasure& m, bool closed) {
    if (m.contours.count == 0)
        return;
    Contour& c = m.contours.last();
    if (closed && c.count > 1 &&
            distanceBetween(m.points.data[c.first], m.points.data[c.first + c.count - 1]) < kCoincident) {
        --c.count;
        m.points.pop();
    }
    if (c.count < 2) {
        m.points.count = c.first;
        m.contours.pop();
        return;
    }
    c.closed = closed;
}

// Returns the cell that a coordinate falls in along an axis, clamped to the grid. Returns the first cell for a
// coordinate that is not a number.
uint32_t cellOf(float at, float origin, float size, uint32_t cells) {
    float cell = prF32Floor((at - origin) / size);
    return !(cell >= 0) ? 0 : cell >= float(cells) ? cells - 1 : uint32_t(cell);
}

// Writes the range of cells that an interval covers along an axis, clamped to the grid.
void cellRange(float low, float high, float origin, float size, uint32_t cells, uint32_t* first, uint32_t* last) {
    *first = cellOf(low, origin, size, cells);
    *last = cellOf(high, origin, size, cells);
}

void buildGrid(PathMeasure& m) {
    uint32_t total = m.segments.count;
    PrRect& b = m.bounds;
    b = PrRect{kPrF32Max, kPrF32Max, -kPrF32Max, -kPrF32Max};
    for (uint32_t i = 0; i < m.points.count; ++i) {
        PrVec2 p = m.points.data[i];
        b.left = prF32Min(b.left, p.x);
        b.top = prF32Min(b.top, p.y);
        b.right = prF32Max(b.right, p.x);
        b.bottom = prF32Max(b.bottom, p.y);
    }
    float side = prF32Ceil(prF32Sqrt(float(total)));
    uint32_t cells = side < 1 ? 1 : side > float(kMaxCells) ? kMaxCells : uint32_t(side);
    m.columns = cells;
    m.rows = cells;
    m.cellWidth = prF32Max(prRectWidth(b) / float(cells), kCoincident);
    m.cellHeight = prF32Max(prRectHeight(b) / float(cells), kCoincident);
    uint32_t count = m.columns * m.rows;
    m.cellStart.reserve(count + 1);
    m.cellStart.count = count + 1;
    for (uint32_t i = 0; i <= count; ++i)
        m.cellStart.data[i] = 0;
    // The first pass counts the segments of each cell, and the second places them.
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t s = 0; s < total; ++s) {
            PrVec2 p = m.points.data[m.segments.data[s].from];
            PrVec2 q = m.points.data[m.segments.data[s].to];
            uint32_t x0 = 0;
            uint32_t x1 = 0;
            uint32_t y0 = 0;
            uint32_t y1 = 0;
            cellRange(prF32Min(p.x, q.x), prF32Max(p.x, q.x), b.left, m.cellWidth, m.columns, &x0, &x1);
            cellRange(prF32Min(p.y, q.y), prF32Max(p.y, q.y), b.top, m.cellHeight, m.rows, &y0, &y1);
            for (uint32_t y = y0; y <= y1 && y < m.rows; ++y) {
                for (uint32_t x = x0; x <= x1 && x < m.columns; ++x) {
                    uint32_t cell = y * m.columns + x;
                    if (pass == 0)
                        ++m.cellStart.data[cell + 1];
                    else
                        m.cellSegments.data[m.cellStart.data[cell]++] = s;
                }
            }
        }
        if (pass == 0) {
            for (uint32_t i = 0; i < count; ++i)
                m.cellStart.data[i + 1] += m.cellStart.data[i];
            uint32_t entries = m.cellStart.data[count];
            m.cellSegments.reserve(entries != 0 ? entries : 1);
            m.cellSegments.count = entries;
        }
    }
    // Placing the segments moved the start of each cell to the start of the next cell. This moves them back.
    for (uint32_t i = count; i > 0; --i)
        m.cellStart.data[i] = m.cellStart.data[i - 1];
    m.cellStart.data[0] = 0;
}

// Returns the index of the contour that holds `distance` along the path, or the earlier contour if two meet at that
// distance. The measure must have contours.
uint32_t contourAt(const PathMeasure& m, float distance) {
    for (uint32_t k = 0; k + 1 < m.contours.count; ++k) {
        const Contour& c = m.contours.data[k];
        if (distance <= c.start + c.length)
            return k;
    }
    return m.contours.count - 1;
}

// Returns the index of the segment of a contour that contains arc length `distance`.
uint32_t segmentAt(const PathMeasure& m, const Contour& c, float distance) {
    // The last segment from the second on that starts at or before `distance`, or the first.
    uint32_t low = partitionPoint(1, c.count, [&](uint32_t i) { return m.along.data[c.first + i] <= distance; }) - 1;
    uint32_t last = c.closed ? c.count - 1 : c.count - 2;
    return low > last ? last : low;
}

void positionOf(const PathMeasure& m, const Contour& c, float distance, PrVec2* point, PrVec2* tangent) {
    distance = prF32Clamp(distance, 0, c.length);
    uint32_t i = segmentAt(m, c, distance);
    PrVec2 a = m.points.data[c.first + i];
    PrVec2 b = m.points.data[c.first + (i + 1) % c.count];
    float start = m.along.data[c.first + i];
    float span = distanceBetween(a, b);
    float t = span > 0 ? prF32Clamp((distance - start) / span, 0, 1) : 0;
    *point = a + (b - a) * t;
    *tangent = span > 0 ? (b - a) / span : PrVec2{1, 0};
}

// Appends the contour's points strictly between two arc lengths. The caller orders the lengths.
void appendBetween(const PathMeasure& m, const Contour& c, float start, float end, PrPathPolyline* out) {
    for (uint32_t i = 0; i < c.count; ++i) {
        float at = m.along.data[c.first + i];
        if (at > start && at < end)
            prPathPolylineLineTo(out, m.points.data[c.first + i]);
    }
}

void measurePolyline(PathMeasure& m, const PrPathPolyline& polyline) {
    m.points.clear();
    m.along.clear();
    m.contours.clear();
    m.segments.clear();
    m.cellStart.clear();
    m.cellSegments.clear();
    m.length = 0;
    for (uint32_t k = 0; k < polyline.contourCount; ++k) {
        uint32_t start = polyline.contours[k];
        uint32_t end = prPathPolylineContourEnd(&polyline, k);
        if (end == start)
            continue;
        m.contours.push(Contour{m.points.count, 0, false, 0, 0});
        for (uint32_t i = start; i < end; ++i)
            addPoint(m, polyline.points[i]);
        endContour(m, prPathPolylineContourIsClosed(&polyline, k));
    }
    for (uint32_t k = 0; k < m.contours.count; ++k) {
        Contour& c = m.contours.data[k];
        float along = 0;
        for (uint32_t i = 0; i < c.count; ++i) {
            if (i != 0)
                along += distanceBetween(m.points.data[c.first + i - 1], m.points.data[c.first + i]);
            m.along.push(along);
        }
        uint32_t segments = c.closed ? c.count : c.count - 1;
        if (c.closed)
            along += distanceBetween(m.points.data[c.first + c.count - 1], m.points.data[c.first]);
        c.start = m.length;
        c.length = along;
        m.length += along;
        for (uint32_t i = 0; i < segments; ++i)
            m.segments.push(Segment{c.first + i, c.first + (i + 1) % c.count, k});
    }
    buildGrid(m);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPathMeasure : PathMeasure {};

PrPathMeasure* prPathMeasureCreate(void) {
    return allocZeroed<PrPathMeasure>();
}

void prPathMeasureDestroy(PrPathMeasure* PR_NULLABLE measure) {
    PathMeasure* m = measure;
    if (m == nullptr)
        return;
    prPathPolylineReset(&m->flat);
    m->points.reset();
    m->along.reset();
    m->contours.reset();
    m->segments.reset();
    m->cellStart.reset();
    m->cellSegments.reset();
    prMemoryFree(m);
}

void prPathMeasureSetPath(PrPathMeasure* measure, const PrPath* path, PrAffine2 transform, float tolerance) {
    prPathPolylineClear(&measure->flat);
    prPathFlatten(path, transform, tolerance, PrPathFlattening_Inside, &measure->flat);
    measurePolyline(*measure, measure->flat);
}

void prPathMeasureSetPolyline(PrPathMeasure* measure, const PrPathPolyline* polyline) {
    measurePolyline(*measure, *polyline);
}

float prPathMeasureLength(const PrPathMeasure* measure) {
    return measure->length;
}

uint32_t prPathMeasureContourCount(const PrPathMeasure* measure) {
    return measure->contours.count;
}

PrPathMeasureContour prPathMeasureContour(const PrPathMeasure* measure, uint32_t index) {
    const Contour& c = measure->contours.data[index];
    return PrPathMeasureContour{c.start, c.length, c.closed};
}

PrPathMeasurePosition prPathMeasurePosition(const PrPathMeasure* measure, float distance) {
    PrPathMeasurePosition out{PrVec2{0, 0}, PrVec2{1, 0}};
    if (measure->contours.count == 0)
        return out;
    distance = prF32Clamp(distance, 0, measure->length);
    const Contour& c = measure->contours.data[contourAt(*measure, distance)];
    positionOf(*measure, c, distance - c.start, &out.point, &out.tangent);
    return out;
}

bool prPathMeasureLineTransform(const PrPathMeasure* measure, float distance, PrAffine2* out) {
    if (measure->contours.count == 0 || !(distance >= 0 && distance <= measure->length))
        return false;
    PrPathMeasurePosition at = prPathMeasurePosition(measure, distance);
    PrVec2 p = at.point;
    PrVec2 t = at.tangent;
    *out = PrAffine2{t.x, t.y, -t.y, t.x, p.x - t.x * distance, p.y - t.y * distance};
    return true;
}

void prPathMeasureSegment(const PrPathMeasure* measure, float start, float end, PrPathPolyline* out) {
    start = prF32Clamp(start, 0, measure->length);
    end = prF32Clamp(end, 0, measure->length);
    if (!(end > start))
        return;
    for (uint32_t k = 0; k < measure->contours.count; ++k) {
        const Contour& c = measure->contours.data[k];
        float from = prF32Max(start, c.start) - c.start;
        float to = prF32Min(end, c.start + c.length) - c.start;
        if (!(to > from))
            continue;
        PrVec2 point{};
        PrVec2 tangent{};
        positionOf(*measure, c, from, &point, &tangent);
        prPathPolylineMoveTo(out, point);
        appendBetween(*measure, c, from, to, out);
        positionOf(*measure, c, to, &point, &tangent);
        prPathPolylineLineTo(out, point);
    }
}

bool prPathMeasureNearest(const PrPathMeasure* measure, PrVec2 point, float reach, PrPathMeasureNearest* out) {
    if (measure->segments.count == 0 || !(reach >= 0))
        return false;
    uint32_t x0 = 0;
    uint32_t x1 = 0;
    uint32_t y0 = 0;
    uint32_t y1 = 0;
    if (point.x + reach < measure->bounds.left || point.x - reach > measure->bounds.right ||
            point.y + reach < measure->bounds.top || point.y - reach > measure->bounds.bottom)
        return false;
    cellRange(point.x - reach, point.x + reach, measure->bounds.left, measure->cellWidth, measure->columns, &x0, &x1);
    cellRange(point.y - reach, point.y + reach, measure->bounds.top, measure->cellHeight, measure->rows, &y0, &y1);
    float best = reach * reach;
    bool found = false;
    uint32_t bestSegment = 0;
    for (uint32_t y = y0; y <= y1 && y < measure->rows; ++y) {
        for (uint32_t x = x0; x <= x1 && x < measure->columns; ++x) {
            uint32_t cell = y * measure->columns + x;
            for (uint32_t k = measure->cellStart.data[cell]; k < measure->cellStart.data[cell + 1]; ++k) {
                uint32_t segment = measure->cellSegments.data[k];
                const Segment& s = measure->segments.data[segment];
                PrVec2 a = measure->points.data[s.from];
                PrVec2 b = measure->points.data[s.to];
                PrVec2 d{b.x - a.x, b.y - a.y};
                float lengthSquared = d.x * d.x + d.y * d.y;
                PrVec2 pa{point.x - a.x, point.y - a.y};
                float t = lengthSquared > 0 ? prF32Clamp((pa.x * d.x + pa.y * d.y) / lengthSquared, 0, 1) : 0;
                PrVec2 near{a.x + d.x * t - point.x, a.y + d.y * t - point.y};
                float squared = near.x * near.x + near.y * near.y;
                bool tied = found && prF32Abs(squared - best) <= kPrPathMeasureTieTolerance;
                if (tied ? segment >= bestSegment : squared > best)
                    continue;
                best = squared;
                found = true;
                bestSegment = segment;
                float length = prF32Sqrt(lengthSquared);
                out->contour = s.contour;
                out->distance = measure->contours.data[s.contour].start + measure->along.data[s.from] + t * length;
                out->offset = length > 0 ? (d.x * pa.y - d.y * pa.x) / length : 0;
            }
        }
    }
    return found;
}
