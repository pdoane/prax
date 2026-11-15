// Hierarchical variation glyphs.
//
// A glyph is a tree of parts: composites, whose subparts are parts, and shapes at the leaves, whose paths are runs of
// quadratic segments. Every part has axes of its own, in [-1, 1]. A glyph's part takes the font's normalized
// coordinates for its axes. A composite gives each part of its tree, below itself, the values of its axes and a
// placement, which are its master values plus its extremum deltas scaled by its own axis values: for each axis, the
// column of the negative extremum scaled by the value's magnitude if it is negative, or else the positive extremum's
// column scaled by the value. The values that a composite gives a part deeper in its tree add to those that the part's
// nearer composites give it.
//
// A placement is a master translation and rotation, and an extremum translation and rotation. One composite's placement
// of a part maps a point p to R(master)(R(extremum) p + extremum translation) + master translation. The placements of a
// part compose from that of the glyph's part, the outermost, which maps the part's points first, to that of the part's
// own composite, which maps them last.
//
// A segment of a shape is an on-curve point and the control point after it. A curve segment stores the position of its
// on-curve point along the line from the control point before it to its own, which keeps the curve smooth there. A
// tangent segment stores its on-curve point, which is moved to the nearest point of that line within the two control
// points. A corner segment's point is where it is stored.

#include "font/src/hvgl.h"

#include "pr/align.h"
#include "pr/bit_cast.h"
#include "pr/float.h"
#include "pr/raw_array.h"

namespace pr_impl {

namespace {

constexpr uint32_t kHvgl = PR_FONT_TAG('h', 'v', 'g', 'l');
constexpr uint32_t kHvpm = PR_FONT_TAG('h', 'v', 'p', 'm');

// The major version of hvgl that the reader reads.
constexpr uint16_t kHvglMajor = 3;

// How deep parts may nest, and how many parts one glyph may visit. They bound the load of a font whose parts refer to
// each other, or that shares parts so widely that a glyph's tree is far larger than the table.
constexpr uint32_t kMaxDepth = 64;
constexpr uint32_t kMaxPartsVisited = 1u << 16;

// The flag of a part's header that makes it a composite.
constexpr uint16_t kCompositeFlag = 0x0001;

// How a shape's segment places its on-curve point.
enum {
    Blend_Curve = 0,
    Blend_Corner = 1,
    Blend_Tangent = 2,
    Blend_TangentFirstOfPair = 3,
    Blend_TangentSecondOfPair = 4,
};
typedef uint8_t Blend;

uint32_t minOf(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

// The little-endian values of the tables, or zero if any byte is past the end.
uint16_t le16(SfntTable table, uint32_t at) {
    if (table.data == nullptr || at >= table.size || table.size - at < 2)
        return 0;
    return uint16_t(table.data[at] | table.data[at + 1] << 8);
}

uint32_t le32(SfntTable table, uint32_t at) {
    if (table.data == nullptr || at >= table.size || table.size - at < 4)
        return 0;
    const uint8_t* p = table.data + at;
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

double leFloat(SfntTable table, uint32_t at) {
    return double(bitCast<float>(le32(table, at)));
}

double leDouble(SfntTable table, uint32_t at) {
    if (table.data == nullptr || at >= table.size || table.size - at < 8)
        return 0;
    uint64_t bits = 0;
    for (uint32_t i = 0; i < 8; ++i)
        bits |= uint64_t(table.data[at + i]) << (8 * i);
    return bitCast<double>(bits);
}

// A map from the points of a part to the glyph's: p maps to (xx px + xy py + x, yx px + yy py + y).
struct Map {
    double xx, yx, xy, yy, x, y;
};

constexpr Map kIdentity = {1, 0, 0, 1, 0, 0};

PrVec2 mapPoint(const Map& m, double x, double y) {
    return PrVec2{float(m.xx * x + m.xy * y + m.x), float(m.yx * x + m.yy * y + m.y)};
}

// Returns `outer` after the rotation by `angle` and then the translation (x, y).
Map mapThen(const Map& outer, double angle, double x, double y) {
    double c = prF64Cos(angle);
    double s = prF64Sin(angle);
    return Map{
        outer.xx * c + outer.xy * s, outer.yx * c + outer.yy * s, outer.xy * c - outer.xx * s,
        outer.yy * c - outer.yx * s, outer.xx * x + outer.xy * y + outer.x, outer.yx * x + outer.yy * y + outer.y,
    };
}

// The placement of a part by the composites of a glyph so far: it rotates by `angle`, then translates by (x, y).
struct Placement {
    double x, y, angle;
};

// One composite's placement of a part of its tree.
struct Step {
    double masterX, masterY, masterAngle;
    double extremumX, extremumY, extremumAngle;
};

// Returns the placement after the composite's step: the step maps the points of the placement.
Placement placeAfter(Placement before, const Step& step) {
    double c = prF64Cos(step.extremumAngle);
    double s = prF64Sin(step.extremumAngle);
    double x = c * before.x - s * before.y + step.extremumX;
    double y = s * before.x + c * before.y + step.extremumY;
    c = prF64Cos(step.masterAngle);
    s = prF64Sin(step.masterAngle);
    return Placement{
        c * x - s * y + step.masterX,
        s * x + c * y + step.masterY,
        before.angle + step.masterAngle + step.extremumAngle,
    };
}

struct Loader {
    const HvglFace& face;
    FontGlyphOutline* out;
    uint32_t visits;
};

// Returns the part that hvpm maps `part` to: its place in a range of the map, from the range's destination.
uint32_t mappedPart(const HvglFace& face, uint32_t part) {
    uint32_t low = 0;
    uint32_t high = face.rangeCount;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t at = 12 * middle;
        uint32_t start = le32(face.ranges, at);
        uint32_t end = le32(face.ranges, at + 4);
        if (part < start) {
            high = middle;
        } else if (part > end) {
            low = middle + 1;
        } else {
            uint32_t mapped = part - start + le32(face.ranges, at + 8);
            return mapped >= part - start && mapped < face.partCount ? mapped : part;
        }
    }
    return part;
}

// Gets the data of the part, after the map, and its offset in the table. Returns false if the part is past the end or
// its data does not fit.
bool partData(const HvglFace& face, uint32_t part, SfntTable* data, uint32_t* offset) {
    part = mappedPart(face, part);
    if (part >= face.partCount)
        return false;
    uint32_t start = le32(face.index, 4 * part);
    uint32_t end = le32(face.index, 4 * part + 4);
    if (end < start + 2 || end > face.index.size)
        return false;
    *data = SfntTable{face.index.data + start, end - start};
    *offset = uint32_t(face.index.data - face.table.data) + start;
    return true;
}

// Appends the paths of a shape at the axis values, mapped by `map`. Values past `axisCount` are 0.
void loadShape(Loader& l, SfntTable data, uint32_t offset, const double* PR_NULLABLE axes, uint32_t axisCount,
        const Map& map) {
    uint32_t axisTotal = le16(data, 2);
    uint32_t pathCount = le16(data, 4);
    uint32_t segmentCount = le16(data, 6);
    uint32_t blends = 8 + 2 * pathCount;
    // The coordinates start on a Float64 boundary of the table.
    uint32_t coords = alignUp(offset + blends + segmentCount, 8u) - offset;
    uint32_t rows = 4 * segmentCount;
    uint64_t end = uint64_t(coords) + uint64_t(8) * rows * (1 + 2 * uint64_t(axisTotal));
    if (end > data.size)
        return;
    uint32_t segments = 0;
    for (uint32_t i = 0; i < pathCount; ++i)
        segments += le16(data, 8 + 2 * i);
    if (segments != segmentCount)
        return;
    RawArray<double> values;
    values.resize(rows);
    for (uint32_t r = 0; r < rows; ++r)
        values.data[r] = leDouble(data, coords + 8 * r);
    for (uint32_t i = 0; i < axisTotal && i < axisCount; ++i) {
        double value = axes[i];
        if (value == 0)
            continue;
        uint32_t column = 2 * i + (value > 0 ? 1 : 0);
        double weight = prF64Abs(value);
        uint32_t at = coords + 8 * rows * (1 + column);
        for (uint32_t r = 0; r < rows; ++r)
            values.data[r] += weight * leDouble(data, at + 8 * r);
    }
    uint32_t first = 0;
    for (uint32_t i = 0; i < pathCount; ++i) {
        uint32_t count = le16(data, 8 + 2 * i);
        // A path of fewer than three segments is not drawn.
        if (count >= 3) {
            for (uint32_t j = 0; j < count; ++j) {
                const double* segment = values.data + 4 * (first + j);
                const double* before = values.data + 4 * (first + (j + count - 1) % count);
                double fromX = before[2];
                double fromY = before[3];
                double toX = segment[2];
                double toY = segment[3];
                double x = segment[0];
                double y = segment[1];
                switch (Blend(data.uint8At(blends + first + j))) {
                    case Blend_Curve:
                        x = fromX + segment[0] * (toX - fromX);
                        y = fromY + segment[0] * (toY - fromY);
                        break;
                    case Blend_Tangent:
                    case Blend_TangentFirstOfPair:
                    case Blend_TangentSecondOfPair: {
                        double dx = toX - fromX;
                        double dy = toY - fromY;
                        double length = dx * dx + dy * dy;
                        double t = length > 0 ? ((x - fromX) * dx + (y - fromY) * dy) / length : 0;
                        t = prF64Min(prF64Max(t, 0.0), 1.0);
                        x = fromX + t * dx;
                        y = fromY + t * dy;
                        break;
                    }
                    default:
                        break;
                }
                fontOutlinePush(*l.out, mapPoint(map, x, y), OutlinePoint_On);
                fontOutlinePush(*l.out, mapPoint(map, toX, toY), OutlinePoint_Quadratic);
            }
            fontOutlineEndContour(*l.out);
        }
        first += count;
    }
}

void loadPart(Loader& l, uint32_t part, uint32_t depth, const double* PR_NULLABLE axes, uint32_t axisCount,
    const double* PR_NULLABLE givenAxes, uint32_t givenAxisCount, const Placement* PR_NULLABLE given,
    uint32_t givenCount, const Map& map);

// Appends the parts of a composite at the axis values, mapped by `map`. The composite's own composites give the rows of
// its tree the axis values `givenAxes` and the placements `given`, and rows past those are 0.
void loadComposite(Loader& l, SfntTable data, uint32_t depth, const double* PR_NULLABLE axes, uint32_t axisCount,
        const double* PR_NULLABLE givenAxes, uint32_t givenAxisCount, const Placement* PR_NULLABLE given,
        uint32_t givenCount, const Map& map) {
    uint32_t axisTotal = le16(data, 2);
    uint32_t subpartCount = le16(data, 4);
    uint32_t treeParts = le16(data, 6);
    uint32_t treeAxes = le16(data, 8);
    uint32_t masterAxisCount = le16(data, 12);
    uint32_t extremumAxisCount = le16(data, 14);
    uint32_t masterTranslationCount = le16(data, 16);
    uint32_t masterRotationCount = le16(data, 18);
    uint32_t extremumTranslationCount = le16(data, 20);
    uint32_t extremumRotationCount = le16(data, 22);
    uint32_t subparts = 4 * uint32_t(le16(data, 24));
    uint32_t columnStarts = 4 * uint32_t(le16(data, 26));
    uint32_t masterAxisDeltas = 4 * uint32_t(le16(data, 28));
    uint32_t extremumAxisDeltas = 4 * uint32_t(le16(data, 30));
    uint32_t translations = 4 * uint32_t(le16(data, 32));
    uint32_t rotations = 4 * uint32_t(le16(data, 34));
    if (treeParts == 0 || treeAxes < axisTotal)
        return;
    // The rows of the tree below the composite: one placement for each part, and one value for each of their axes.
    uint32_t rowCount = treeParts - 1;
    uint32_t axisRows = treeAxes - axisTotal;
    uint32_t columns = 2 * axisTotal;
    RawArray<double> weights;
    weights.resize(columns);
    for (uint32_t i = 0; i < axisTotal; ++i) {
        double value = i < axisCount ? axes[i] : 0;
        weights.data[2 * i] = value < 0 ? -value : 0;
        weights.data[2 * i + 1] = value > 0 ? value : 0;
    }

    RawArray<double> values;
    values.resize(axisRows);
    for (uint32_t r = 0; r < axisRows; ++r)
        values.data[r] = r < givenAxisCount ? givenAxes[r] : 0;
    uint32_t masterRows = columnStarts + 2 * (columns + 1);
    uint32_t extremumRows = masterRows + 2 * masterAxisCount;
    for (uint32_t i = 0; i < masterAxisCount; ++i) {
        uint32_t row = le16(data, masterRows + 2 * i);
        if (row < axisRows)
            values.data[row] += leFloat(data, masterAxisDeltas + 4 * i);
    }
    for (uint32_t c = 0; c < columns; ++c) {
        double weight = weights.data[c];
        uint32_t start = le16(data, columnStarts + 2 * c);
        uint32_t end = le16(data, columnStarts + 2 * c + 2);
        if (weight == 0 || end > extremumAxisCount)
            continue;
        for (uint32_t k = start; k < end; ++k) {
            uint32_t row = le16(data, extremumRows + 2 * k);
            if (row < axisRows)
                values.data[row] += weight * leFloat(data, extremumAxisDeltas + 4 * k);
        }
    }

    RawArray<Step> steps;
    steps.resize(rowCount);
    for (uint32_t r = 0; r < rowCount; ++r)
        steps.data[r] = Step{};
    uint32_t masterTranslations = translations;
    uint32_t extremumTranslations = masterTranslations + 8 * masterTranslationCount;
    uint32_t extremumTranslationIndices = extremumTranslations + 8 * extremumTranslationCount;
    uint32_t masterTranslationIndices = extremumTranslationIndices + 4 * extremumTranslationCount;
    for (uint32_t i = 0; i < masterTranslationCount; ++i) {
        uint32_t row = le16(data, masterTranslationIndices + 2 * i);
        if (row < rowCount) {
            steps.data[row].masterX += leFloat(data, masterTranslations + 8 * i);
            steps.data[row].masterY += leFloat(data, masterTranslations + 8 * i + 4);
        }
    }
    for (uint32_t i = 0; i < extremumTranslationCount; ++i) {
        uint32_t row = le16(data, extremumTranslationIndices + 4 * i);
        uint32_t column = le16(data, extremumTranslationIndices + 4 * i + 2);
        if (row < rowCount && column < columns && weights.data[column] != 0) {
            steps.data[row].extremumX += weights.data[column] * leFloat(data, extremumTranslations + 8 * i);
            steps.data[row].extremumY += weights.data[column] * leFloat(data, extremumTranslations + 8 * i + 4);
        }
    }
    uint32_t masterRotations = rotations;
    uint32_t extremumRotations = masterRotations + 4 * masterRotationCount;
    uint32_t extremumRotationIndices = extremumRotations + 4 * extremumRotationCount;
    uint32_t masterRotationIndices = extremumRotationIndices + 4 * extremumRotationCount;
    for (uint32_t i = 0; i < masterRotationCount; ++i) {
        uint32_t row = le16(data, masterRotationIndices + 2 * i);
        if (row < rowCount)
            steps.data[row].masterAngle += leFloat(data, masterRotations + 4 * i);
    }
    for (uint32_t i = 0; i < extremumRotationCount; ++i) {
        uint32_t row = le16(data, extremumRotationIndices + 4 * i);
        uint32_t column = le16(data, extremumRotationIndices + 4 * i + 2);
        if (row < rowCount && column < columns)
            steps.data[row].extremumAngle += weights.data[column] * leFloat(data, extremumRotations + 4 * i);
    }
    RawArray<Placement> placements;
    placements.resize(rowCount);
    for (uint32_t r = 0; r < rowCount; ++r)
        placements.data[r] = placeAfter(r < givenCount ? given[r] : Placement{0, 0, 0}, steps.data[r]);

    for (uint32_t s = 0; s < subpartCount; ++s) {
        uint32_t at = subparts + 8 * s;
        uint32_t part = le32(data, at);
        uint32_t row = le16(data, at + 4);
        uint32_t axisRow = le16(data, at + 6);
        if (row >= rowCount || axisRow > axisRows)
            continue;
        const Placement& placement = placements.data[row];
        Map placed = mapThen(map, placement.angle, placement.x, placement.y);
        // The subpart's axes come first among its rows, then those of its tree, after its own placement.
        const double* own = values.data + axisRow;
        uint32_t left = axisRows - axisRow;
        loadPart(l, part, depth + 1, own, left, own, left, placements.data + row + 1, rowCount - row - 1, placed);
    }
}

// Appends the part at the axis values, mapped by `map`, with the axis values and placements that the composites above
// it give the rows of its tree, as loadComposite takes them. `givenAxes` starts at the part's own axes, which the part
// skips.
void loadPart(Loader& l, uint32_t part, uint32_t depth, const double* PR_NULLABLE axes, uint32_t axisCount,
        const double* PR_NULLABLE givenAxes, uint32_t givenAxisCount, const Placement* PR_NULLABLE given,
        uint32_t givenCount, const Map& map) {
    SfntTable data;
    uint32_t offset = 0;
    if (depth > kMaxDepth || l.visits >= kMaxPartsVisited || !partData(l.face, part, &data, &offset))
        return;
    ++l.visits;
    uint32_t own = le16(data, 2);
    if ((le16(data, 0) & kCompositeFlag) == 0) {
        loadShape(l, data, offset, axes, minOf(own, axisCount), map);
        return;
    }
    uint32_t skip = minOf(own, givenAxisCount);
    loadComposite(l, data, depth, axes, minOf(own, axisCount), givenAxes != nullptr ? givenAxes + skip : nullptr,
        givenAxisCount - skip, given, givenCount, map);
}

} // namespace

bool hvglOpen(const Sfnt& sfnt, HvglFace* out) {
    *out = HvglFace{};
    SfntTable table = sfntTable(sfnt, kHvgl);
    if (le16(table, 0) != kHvglMajor)
        return false;
    uint32_t partCount = le32(table, 8);
    uint32_t indexOffset = le32(table, 12);
    uint32_t glyphCount = le32(table, 16);
    SfntTable index = table.from(indexOffset);
    if (partCount == 0 || glyphCount > partCount || index.size / 4 <= partCount)
        return false;
    out->table = table;
    out->index = index;
    out->partCount = partCount;
    out->glyphCount = glyphCount;
    SfntTable map = sfntTable(sfnt, kHvpm);
    SfntTable ranges = map.from(le32(map, 4));
    uint32_t rangeCount = le32(ranges, 0);
    if (le16(map, 0) == 1 && ranges.size >= 4 && rangeCount <= (ranges.size - 4) / 12) {
        out->ranges = ranges.from(4);
        out->rangeCount = rangeCount;
    }
    return true;
}

bool hvglLoad(const HvglFace& face, SfntCoords coords, uint32_t glyph, FontGlyphOutline* out) {
    fontOutlineClear(*out);
    if (glyph >= face.glyphCount)
        return false;
    double axes[64];
    uint32_t axisCount = minOf(coords.count, 64);
    for (uint32_t i = 0; i < axisCount; ++i)
        axes[i] = double(coords.values[i]);
    Loader l{face, out, 0};
    loadPart(l, glyph, 0, axes, axisCount, nullptr, 0, nullptr, 0, kIdentity);
    return true;
}

} // namespace pr_impl
