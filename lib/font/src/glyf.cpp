// TrueType outlines.

#include "font/src/glyf.h"

#include "pr/float.h"

namespace pr_impl {

namespace {

constexpr uint32_t kGlyf = 0x676c7966;           // 'glyf'
constexpr uint32_t kLoca = 0x6c6f6361;           // 'loca'
constexpr uint32_t kGvar = 0x67766172;           // 'gvar'
constexpr uint32_t kHead = 0x68656164;           // 'head'

// How deep composite glyphs may nest. It bounds the load of a font whose components refer to each other.
constexpr uint32_t kMaxComponentDepth = 16;

// The flags of a component of a composite glyph.
enum {
    ComponentFlag_ArgsAreWords = 0x0001,
    ComponentFlag_ArgsAreOffsets = 0x0002,
    ComponentFlag_Scale = 0x0008,
    ComponentFlag_MoreComponents = 0x0020,
    ComponentFlag_ScaleXY = 0x0040,
    ComponentFlag_TwoByTwo = 0x0080,
    ComponentFlag_UseMyMetrics = 0x0200,
    ComponentFlag_ScaledOffset = 0x0800,
};
typedef uint16_t ComponentFlags;

// The flags of a point of a simple glyph.
enum {
    PointFlag_OnCurve = 0x01,
    PointFlag_XShort = 0x02,
    PointFlag_YShort = 0x04,
    PointFlag_Repeat = 0x08,
    PointFlag_XSameOrPositive = 0x10,
    PointFlag_YSameOrPositive = 0x20,
};
typedef uint8_t PointFlags;

// The flags of a tuple variation header of gvar.
enum {
    TupleFlag_EmbeddedPeak = 0x8000,
    TupleFlag_Intermediate = 0x4000,
    TupleFlag_PrivatePoints = 0x2000,
    TupleFlag_IndexMask = 0x0fff,
    TupleFlag_SharedPoints = 0x8000,        // in the count of a glyph's variation data
    TupleFlag_CountMask = 0x0fff,
};
typedef uint16_t TupleFlags;

// The flags of a run of packed point numbers and deltas.
enum {
    RunFlag_PointsAreWords = 0x80,
    RunFlag_PointCountMask = 0x7f,
    RunFlag_DeltasAreZero = 0x80,
    RunFlag_DeltasAreWords = 0x40,
    RunFlag_DeltaCountMask = 0x3f,
};
typedef uint8_t RunFlags;

// The bytes of the glyph's data, or an empty table if the glyph has none.
SfntTable glyphData(const GlyfFace& face, uint32_t glyph) {
    uint32_t start = face.longOffsets ? face.loca.uint32At(glyph * 4) : uint32_t(face.loca.uint16At(glyph * 2)) * 2;
    uint32_t end = face.longOffsets ? face.loca.uint32At(glyph * 4 + 4)
                                    : uint32_t(face.loca.uint16At(glyph * 2 + 2)) * 2;
    if (end <= start || end > face.glyf.size)
        return SfntTable{nullptr, 0};
    return SfntTable{face.glyf.data + start, end - start};
}

// A reader of the serialized data of gvar, which reads zero past its end.
struct Reader {
    SfntTable table;
    uint32_t at;

    uint8_t byte() {
        return table.uint8At(at++);
    }
    uint16_t word() {
        at += 2;
        return table.uint16At(at - 2);
    }
};

// Reads packed point numbers. Returns false for "all points", which an empty list stands for.
bool readPoints(Reader& r, Array<uint16_t>& points) {
    points.clear();
    uint32_t count = r.byte();
    if (count == 0)
        return false;
    if ((count & 0x80) != 0)
        count = (count & 0x7f) << 8 | r.byte();
    uint16_t point = 0;
    while (points.count < count && r.at < r.table.size) {
        uint8_t run = r.byte();
        uint32_t length = uint32_t(run & RunFlag_PointCountMask) + 1;
        for (uint32_t i = 0; i < length && points.count < count; ++i) {
            point = uint16_t(point + ((run & RunFlag_PointsAreWords) != 0 ? r.word() : r.byte()));
            points.push(point);
        }
    }
    return true;
}

// Reads `count` packed deltas into `out`.
void readDeltas(Reader& r, uint32_t count, Array<float>& out) {
    out.clear();
    while (out.count < count) {
        uint8_t run = r.byte();
        uint32_t length = uint32_t(run & RunFlag_DeltaCountMask) + 1;
        for (uint32_t i = 0; i < length && out.count < count; ++i) {
            if ((run & RunFlag_DeltasAreZero) != 0)
                out.push(0);
            else if ((run & RunFlag_DeltasAreWords) != 0)
                out.push(float(int16_t(r.word())));
            else
                out.push(float(int8_t(r.byte())));
        }
        if (r.at >= r.table.size && (run & RunFlag_DeltasAreZero) == 0)
            break;
    }
    while (out.count < count)
        out.push(0);
}

// Interpolates the deltas of the untouched points from `first` to `last` of one axis, between the touched reference
// points `a` and `b`, as the IUP instruction does: a point outside the references' span takes the delta of the nearer
// reference, and a point inside it a delta in proportion. References at one position with different deltas move
// nothing.
void interpolate(uint32_t first, uint32_t last, uint32_t a, uint32_t b, const PrVec2* original, PrVec2* deltas,
                 uint32_t axis) {
    if (first > last)
        return;
    auto component = [axis](const PrVec2& v) -> float { return axis == 0 ? v.x : v.y; };
    float in1 = component(original[a]);
    float in2 = component(original[b]);
    float d1 = component(deltas[a]);
    float d2 = component(deltas[b]);
    if (in1 > in2) {
        float t = in1;
        in1 = in2;
        in2 = t;
        t = d1;
        d1 = d2;
        d2 = t;
    }
    if (in1 == in2 && d1 != d2)
        return;
    float scale = in1 != in2 ? ((in2 + d2) - (in1 + d1)) / (in2 - in1) : 0;
    for (uint32_t p = first; p <= last; ++p) {
        float in = component(original[p]);
        float delta = in <= in1 ? d1 : in >= in2 ? d2 : (in1 + d1) + (in - in1) * scale - in;
        if (axis == 0)
            deltas[p].x = delta;
        else
            deltas[p].y = delta;
    }
}

// Infers the deltas of the points that a tuple leaves untouched, contour by contour.
void inferDeltas(const PrVec2* original, PrVec2* deltas, const bool* touched, const uint32_t* ends,
                 uint32_t contourCount) {
    uint32_t first = 0;
    for (uint32_t c = 0; c < contourCount; ++c) {
        uint32_t end = ends[c] - 1;
        uint32_t point = first;
        while (point <= end && !touched[point])
            ++point;
        if (point > end) {
            first = end + 1;
            continue;
        }
        uint32_t firstTouched = point;
        uint32_t current = point;
        for (++point; point <= end; ++point) {
            if (!touched[point])
                continue;
            for (uint32_t axis = 0; axis < 2; ++axis)
                interpolate(current + 1, point - 1, current, point, original, deltas, axis);
            current = point;
        }
        if (current == firstTouched) {
            // A single touched point shifts the whole contour.
            for (uint32_t p = first; p <= end; ++p) {
                if (p != current)
                    deltas[p] = deltas[current];
            }
        } else {
            for (uint32_t axis = 0; axis < 2; ++axis) {
                interpolate(current + 1, end, current, firstTouched, original, deltas, axis);
                if (firstTouched > first)
                    interpolate(first, firstTouched - 1, current, firstTouched, original, deltas, axis);
            }
        }
        first = end + 1;
    }
}

// Returns the scalar of a tuple at the coordinates. Without an intermediate region, the region runs from 0 to the peak.
float tupleScalar(SfntTable peaks, SfntTable starts, SfntTable ends, bool intermediate, uint32_t axisCount,
                  SfntCoords coords) {
    float scalar = 1;
    for (uint32_t axis = 0; axis < axisCount && scalar != 0; ++axis) {
        float peak = peaks.f2dot14(axis * 2);
        if (peak == 0)
            continue;
        float coord = axis < coords.count && coords.values != nullptr ? coords.values[axis] : 0;
        if (coord == 0)
            return 0;
        if (coord == peak)
            continue;
        if (!intermediate) {
            if (coord < minF32(0, peak) || coord > maxF32(0, peak))
                return 0;
            scalar *= coord / peak;
            continue;
        }
        float start = starts.f2dot14(axis * 2);
        float end = ends.f2dot14(axis * 2);
        if (coord < start || coord > end)
            return 0;
        scalar *= coord < peak ? (coord - start) / (peak - start) : (end - coord) / (end - peak);
    }
    return scalar;
}

// Adds to `deltas` the variations of gvar for the glyph's `count` points, the last four of which are its phantom
// points. A simple glyph infers the deltas of the points that a tuple leaves out, within `ends`.
void addDeltas(const GlyfFace& face, SfntCoords coords, uint32_t glyph, const PrVec2* original, uint32_t count,
               const uint32_t* PR_NULLABLE ends, uint32_t contourCount, PrVec2* deltas) {
    const SfntTable& gvar = face.gvar;
    if (gvar.size == 0 || coords.count == 0 || glyph >= gvar.uint16At(12))
        return;
    uint32_t axisCount = gvar.uint16At(4);
    uint32_t sharedTuples = gvar.uint32At(8);
    bool longOffsets = (gvar.uint16At(14) & 1) != 0;
    uint32_t array = gvar.uint32At(16);
    auto offset = [&](uint32_t index) {
        return array + (longOffsets ? gvar.uint32At(20 + index * 4) : uint32_t(gvar.uint16At(20 + index * 2)) * 2);
    };
    uint32_t start = offset(glyph);
    uint32_t end = offset(glyph + 1);
    if (end <= start || end > gvar.size)
        return;
    SfntTable data{gvar.data + start, end - start};
    uint16_t tuples = data.uint16At(0);
    Reader serialized{data, data.uint16At(2)};
    Array<uint16_t> shared;
    bool sharedAll = true;
    if ((tuples & TupleFlag_SharedPoints) != 0)
        sharedAll = !readPoints(serialized, shared);
    Array<uint16_t> own;
    Array<float> xs;
    Array<float> ys;
    Array<PrVec2> tupleDeltas;
    Array<bool> touched;
    uint32_t header = 4;
    for (uint32_t t = 0; t < (tuples & TupleFlag_CountMask); ++t) {
        uint16_t size = data.uint16At(header);
        uint16_t index = data.uint16At(header + 2);
        header += 4;
        SfntTable peaks{nullptr, 0};
        if ((index & TupleFlag_EmbeddedPeak) != 0) {
            peaks = data.from(header);
            header += axisCount * 2;
        } else {
            peaks = gvar.from(sharedTuples + (index & TupleFlag_IndexMask) * axisCount * 2);
        }
        bool intermediate = (index & TupleFlag_Intermediate) != 0;
        SfntTable starts{nullptr, 0};
        SfntTable stops{nullptr, 0};
        if (intermediate) {
            starts = data.from(header);
            stops = data.from(header + axisCount * 2);
            header += axisCount * 4;
        }
        Reader r{data, serialized.at};
        serialized.at += size;
        float scalar = tupleScalar(peaks, starts, stops, intermediate, axisCount, coords);
        if (scalar == 0)
            continue;
        bool all = sharedAll;
        const Array<uint16_t>* points = &shared;
        if ((index & TupleFlag_PrivatePoints) != 0) {
            all = !readPoints(r, own);
            points = &own;
        }
        uint32_t listed = all ? count : points->count;
        readDeltas(r, listed, xs);
        readDeltas(r, listed, ys);
        if (all) {
            for (uint32_t i = 0; i < count; ++i) {
                deltas[i].x += xs.data[i] * scalar;
                deltas[i].y += ys.data[i] * scalar;
            }
            continue;
        }
        if (ends == nullptr) {
            for (uint32_t i = 0; i < listed; ++i) {
                uint16_t p = points->data[i];
                if (p < count) {
                    deltas[p].x += xs.data[i] * scalar;
                    deltas[p].y += ys.data[i] * scalar;
                }
            }
            continue;
        }
        // A simple glyph infers the deltas of its untouched points. Phantom points are never inferred.
        tupleDeltas.clear();
        touched.clear();
        for (uint32_t i = 0; i < count; ++i) {
            tupleDeltas.push(PrVec2{0, 0});
            touched.push(false);
        }
        for (uint32_t i = 0; i < listed; ++i) {
            uint16_t p = points->data[i];
            if (p < count) {
                tupleDeltas.data[p] = PrVec2{xs.data[i] * scalar, ys.data[i] * scalar};
                touched.data[p] = true;
            }
        }
        inferDeltas(original, tupleDeltas.data, touched.data, ends, contourCount);
        for (uint32_t i = 0; i < count; ++i) {
            deltas[i].x += tupleDeltas.data[i].x;
            deltas[i].y += tupleDeltas.data[i].y;
        }
    }
    shared.reset();
    own.reset();
    xs.reset();
    ys.reset();
    tupleDeltas.reset();
    touched.reset();
}

// Sets the phantom points of a glyph from its metrics: the horizontal origin at its minimum x less its left side
// bearing, and the vertical origin at its top plus its top side bearing.
GlyfPhantom phantomOf(const GlyfFace& face, SfntCoords coords, uint32_t glyph, int32_t xMin, int32_t yMax) {
    int32_t advance = 0;
    int32_t lsb = 0;
    fontGlyphMetricsHorizontal(*face.metrics, coords, glyph, &advance, &lsb);
    int32_t height = 0;
    int32_t tsb = 0;
    fontGlyphMetricsVertical(*face.metrics, coords, glyph, yMax, &height, &tsb);
    float origin = float(xMin - lsb);
    float top = float(yMax + tsb);
    return GlyfPhantom{PrVec2{origin, 0}, PrVec2{origin + float(advance), 0}, PrVec2{0, top},
                       PrVec2{0, top - float(height)}};
}

// Applies phantom deltas, unless HVAR or VVAR varies the advances instead.
void movePhantom(const GlyfFace& face, GlyfPhantom* phantom, const PrVec2* deltas) {
    if (!fontGlyphMetricsVariesHorizontal(*face.metrics)) {
        phantom->horizontalOrigin.x += deltas[0].x;
        phantom->horizontalOrigin.y += deltas[0].y;
        phantom->horizontalAdvance.x += deltas[1].x;
        phantom->horizontalAdvance.y += deltas[1].y;
    }
    if (!fontGlyphMetricsVariesVertical(*face.metrics)) {
        phantom->verticalOrigin.x += deltas[2].x;
        phantom->verticalOrigin.y += deltas[2].y;
        phantom->verticalAdvance.x += deltas[3].x;
        phantom->verticalAdvance.y += deltas[3].y;
    }
}

bool loadGlyph(const GlyfFace& face, SfntCoords coords, uint32_t glyph, uint32_t depth, FontGlyphOutline* out,
               GlyfPhantom* phantom);

// Appends a simple glyph's contours. `count` is its number of contours.
bool loadSimple(const GlyfFace& face, SfntCoords coords, uint32_t glyph, SfntTable data, uint32_t count,
                FontGlyphOutline* out, GlyfPhantom* phantom) {
    uint32_t base = out->points.count;
    uint32_t pointCount = count != 0 ? uint32_t(data.uint16At(10 + (count - 1) * 2)) + 1 : 0;
    uint32_t instructions = data.uint16At(10 + count * 2);
    uint32_t at = 12 + count * 2 + instructions;
    // Every point takes at least one flag byte, so a count past the data is malformed.
    if (at + pointCount > data.size)
        return false;
    Array<uint8_t> flags;
    while (flags.count < pointCount) {
        uint8_t flag = data.uint8At(at++);
        flags.push(flag);
        if ((flag & PointFlag_Repeat) != 0) {
            uint32_t repeat = data.uint8At(at++);
            for (uint32_t i = 0; i < repeat && flags.count < pointCount; ++i)
                flags.push(flag);
        }
    }
    int32_t x = 0;
    for (uint32_t i = 0; i < pointCount; ++i) {
        uint8_t flag = flags.data[i];
        if ((flag & PointFlag_XShort) != 0) {
            int32_t d = data.uint8At(at++);
            x += (flag & PointFlag_XSameOrPositive) != 0 ? d : -d;
        } else if ((flag & PointFlag_XSameOrPositive) == 0) {
            x += data.int16At(at);
            at += 2;
        }
        fontOutlinePush(*out, PrVec2{float(x), 0}, (flag & PointFlag_OnCurve) != 0 ? OutlinePoint_On
                                                                            : OutlinePoint_Quadratic);
    }
    int32_t y = 0;
    for (uint32_t i = 0; i < pointCount; ++i) {
        uint8_t flag = flags.data[i];
        if ((flag & PointFlag_YShort) != 0) {
            int32_t d = data.uint8At(at++);
            y += (flag & PointFlag_YSameOrPositive) != 0 ? d : -d;
        } else if ((flag & PointFlag_YSameOrPositive) == 0) {
            y += data.int16At(at);
            at += 2;
        }
        out->points.data[base + i].y = float(y);
    }
    flags.reset();
    for (uint32_t c = 0; c < count; ++c) {
        uint32_t end = uint32_t(data.uint16At(10 + c * 2)) + 1;
        if (end > pointCount || (c != 0 && end <= out->ends.last() - base))
            return false;
        out->ends.push(base + end);
    }
    *phantom = phantomOf(face, coords, glyph, data.int16At(2), data.int16At(8));
    if (face.gvar.size == 0 || coords.count == 0)
        return true;
    // The points and the phantom points vary together.
    Array<PrVec2> original;
    Array<PrVec2> deltas;
    Array<uint32_t> ends;
    original.append(out->points.data + base, pointCount);
    original.push(phantom->horizontalOrigin);
    original.push(phantom->horizontalAdvance);
    original.push(phantom->verticalOrigin);
    original.push(phantom->verticalAdvance);
    for (uint32_t i = 0; i < original.count; ++i)
        deltas.push(PrVec2{0, 0});
    for (uint32_t c = 0; c < count; ++c)
        ends.push(out->ends.data[out->ends.count - count + c] - base);
    addDeltas(face, coords, glyph, original.data, original.count, ends.data, count, deltas.data);
    for (uint32_t i = 0; i < pointCount; ++i) {
        out->points.data[base + i].x += deltas.data[i].x;
        out->points.data[base + i].y += deltas.data[i].y;
    }
    movePhantom(face, phantom, deltas.data + pointCount);
    original.reset();
    deltas.reset();
    ends.reset();
    return true;
}

// A component of a composite glyph.
struct Component {
    ComponentFlags flags;
    uint16_t glyph;
    PrVec2 arguments;               // the offset, or the two point numbers that match
    PrAffine2 transform;
};

// Appends a composite glyph's components.
bool loadComposite(const GlyfFace& face, SfntCoords coords, uint32_t glyph, SfntTable data, uint32_t depth,
                   FontGlyphOutline* out, GlyfPhantom* phantom) {
    Array<Component> components;
    uint32_t at = 10;
    ComponentFlags flags = ComponentFlag_MoreComponents;
    while ((flags & ComponentFlag_MoreComponents) != 0 && at + 4 <= data.size) {
        Component c{};
        flags = data.uint16At(at);
        c.flags = flags;
        c.glyph = data.uint16At(at + 2);
        at += 4;
        if ((flags & ComponentFlag_ArgsAreWords) != 0) {
            bool offsets = (flags & ComponentFlag_ArgsAreOffsets) != 0;
            c.arguments = offsets ? PrVec2{float(data.int16At(at)), float(data.int16At(at + 2))}
                                  : PrVec2{float(data.uint16At(at)), float(data.uint16At(at + 2))};
            at += 4;
        } else {
            bool offsets = (flags & ComponentFlag_ArgsAreOffsets) != 0;
            c.arguments = offsets ? PrVec2{float(int8_t(data.uint8At(at))), float(int8_t(data.uint8At(at + 1)))}
                                  : PrVec2{float(data.uint8At(at)), float(data.uint8At(at + 1))};
            at += 2;
        }
        c.transform = PrAffine2{1, 0, 0, 1, 0, 0};
        if ((flags & ComponentFlag_Scale) != 0) {
            c.transform.a = c.transform.d = data.f2dot14(at);
            at += 2;
        } else if ((flags & ComponentFlag_ScaleXY) != 0) {
            c.transform.a = data.f2dot14(at);
            c.transform.d = data.f2dot14(at + 2);
            at += 4;
        } else if ((flags & ComponentFlag_TwoByTwo) != 0) {
            c.transform.a = data.f2dot14(at);
            c.transform.b = data.f2dot14(at + 2);
            c.transform.c = data.f2dot14(at + 4);
            c.transform.d = data.f2dot14(at + 6);
            at += 8;
        }
        components.push(c);
    }
    *phantom = phantomOf(face, coords, glyph, data.int16At(2), data.int16At(8));
    if (face.gvar.size != 0 && coords.count != 0) {
        // The offsets of the components vary as points do, by whole units, and so do the phantom points.
        Array<PrVec2> original;
        Array<PrVec2> deltas;
        for (uint32_t i = 0; i < components.count; ++i)
            original.push(components.data[i].arguments);
        original.push(phantom->horizontalOrigin);
        original.push(phantom->horizontalAdvance);
        original.push(phantom->verticalOrigin);
        original.push(phantom->verticalAdvance);
        for (uint32_t i = 0; i < original.count; ++i)
            deltas.push(PrVec2{0, 0});
        addDeltas(face, coords, glyph, original.data, original.count, nullptr, 0, deltas.data);
        for (uint32_t i = 0; i < original.count; ++i)
            deltas.data[i] = PrVec2{roundF32(deltas.data[i].x), roundF32(deltas.data[i].y)};
        for (uint32_t i = 0; i < components.count; ++i) {
            Component& c = components.data[i];
            if ((c.flags & ComponentFlag_ArgsAreOffsets) != 0)
                c.arguments = PrVec2{c.arguments.x + deltas.data[i].x, c.arguments.y + deltas.data[i].y};
        }
        movePhantom(face, phantom, deltas.data + components.count);
        original.reset();
        deltas.reset();
    }
    uint32_t start = out->points.count;
    bool loaded = true;
    for (uint32_t i = 0; i < components.count && loaded; ++i) {
        const Component& c = components.data[i];
        uint32_t base = out->points.count;
        GlyfPhantom own{};
        loaded = loadGlyph(face, coords, c.glyph, depth + 1, out, &own);
        if (!loaded)
            break;
        bool scaled = (c.flags & (ComponentFlag_Scale | ComponentFlag_ScaleXY | ComponentFlag_TwoByTwo)) != 0;
        if (scaled)
            fontOutlineTransform(*out, base, c.transform);
        PrVec2 offset{0, 0};
        if ((c.flags & ComponentFlag_ArgsAreOffsets) != 0) {
            offset = c.arguments;
            if (scaled && (c.flags & ComponentFlag_ScaledOffset) != 0) {
                offset.x *= sqrtF32(c.transform.a * c.transform.a + c.transform.c * c.transform.c);
                offset.y *= sqrtF32(c.transform.d * c.transform.d + c.transform.b * c.transform.b);
            }
        } else {
            // The component's point `arguments.y` meets the point `arguments.x` of the components before it.
            uint32_t k = start + uint32_t(c.arguments.x);
            uint32_t l = base + uint32_t(c.arguments.y);
            if (k >= base || l >= out->points.count) {
                loaded = false;
                break;
            }
            offset = PrVec2{out->points.data[k].x - out->points.data[l].x,
                            out->points.data[k].y - out->points.data[l].y};
        }
        if (offset.x != 0 || offset.y != 0)
            fontOutlineTransform(*out, base, PrAffine2{1, 0, 0, 1, offset.x, offset.y});
        if ((c.flags & ComponentFlag_UseMyMetrics) != 0)
            *phantom = own;
    }
    components.reset();
    return loaded;
}

bool loadGlyph(const GlyfFace& face, SfntCoords coords, uint32_t glyph, uint32_t depth, FontGlyphOutline* out,
               GlyfPhantom* phantom) {
    if (glyph >= face.glyphCount || depth > kMaxComponentDepth)
        return false;
    SfntTable data = glyphData(face, glyph);
    if (data.size < 10) {
        *phantom = phantomOf(face, coords, glyph, 0, 0);
        if (face.gvar.size != 0 && coords.count != 0) {
            PrVec2 original[4] = {phantom->horizontalOrigin, phantom->horizontalAdvance, phantom->verticalOrigin,
                                  phantom->verticalAdvance};
            PrVec2 deltas[4] = {};
            addDeltas(face, coords, glyph, original, 4, nullptr, 0, deltas);
            movePhantom(face, phantom, deltas);
        }
        return true;
    }
    int16_t contours = data.int16At(0);
    if (contours >= 0)
        return loadSimple(face, coords, glyph, data, uint32_t(contours), out, phantom);
    return loadComposite(face, coords, glyph, data, depth, out, phantom);
}

} // namespace

bool glyfOpen(const Sfnt& sfnt, const FontGlyphMetricTables* metrics, uint32_t glyphCount, GlyfFace* out) {
    *out = GlyfFace{};
    out->glyf = sfntTable(sfnt, kGlyf);
    out->loca = sfntTable(sfnt, kLoca);
    if (out->glyf.size == 0 || out->loca.size == 0)
        return false;
    out->gvar = sfntTable(sfnt, kGvar);
    if (out->gvar.uint16At(0) != 1)
        out->gvar = SfntTable{nullptr, 0};
    out->longOffsets = sfntTable(sfnt, kHead).int16At(50) != 0;
    uint32_t entries = out->loca.size / (out->longOffsets ? 4 : 2);
    out->glyphCount = entries > 0 && entries - 1 < glyphCount ? entries - 1 : glyphCount;
    out->metrics = metrics;
    return true;
}

bool glyfLoad(const GlyfFace& face, SfntCoords coords, uint32_t glyph, FontGlyphOutline* out, GlyfPhantom* phantom) {
    fontOutlineClear(*out);
    *phantom = GlyfPhantom{};
    if (!loadGlyph(face, coords, glyph, 0, out, phantom)) {
        fontOutlineClear(*out);
        return false;
    }
    float origin = phantom->horizontalOrigin.x;
    if (origin != 0) {
        fontOutlineTransform(*out, 0, PrAffine2{1, 0, 0, 1, -origin, 0});
        phantom->horizontalOrigin.x -= origin;
        phantom->horizontalAdvance.x -= origin;
    }
    return true;
}

} // namespace pr_impl
