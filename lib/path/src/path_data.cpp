// Path data: paths written in the SVG path syntax.

#include "pr/path_data.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/number_list.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

bool at(PrStringView text, size_t pos, char c) {
    return pos < text.size && text.data[pos] == c;
}

// Reads an arc flag: a lone 0 or 1, which needs no separator before the next number.
bool readFlag(PrStringView text, size_t* pos, float* out) {
    numberListSkipSpace(text, pos);
    if (!at(text, *pos, '0') && !at(text, *pos, '1'))
        return false;
    *out = text.data[*pos] == '1' ? 1.0f : 0.0f;
    ++*pos;
    return true;
}

char upperOf(char letter) {
    return letter >= 'a' ? char(letter - 'a' + 'A') : letter;
}

// Reads path data one command at a time, as SVG2/paths.html#PathDataBNF gives it.
struct DataReader {
    PrStringView data;
    size_t pos;
    char command;               // the letter of the last command, which arguments without a letter repeat
    bool failed;                // true once the reader stops at an error
};

// Reads the next command into `out`. Returns false at the end of the data or at its first error.
bool readCommand(DataReader& r, PrPathDataCommand* out) {
    numberListSkipSpace(r.data, &r.pos);
    if (r.pos == r.data.size)
        return false;
    r.failed = true;
    char c = r.data.data[r.pos];
    bool first = r.command == 0;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        r.command = c;
        ++r.pos;
    } else if (first || upperOf(r.command) == 'Z') {
        return false;
    } else {
        numberListSkipSeparator(r.data, &r.pos);
    }
    // Data starts with a moveto.
    if (first && upperOf(r.command) != 'M')
        return false;
    PrPathDataCommand command{};
    command.letter = r.command;
    float* n = command.arguments;
    switch (upperOf(r.command)) {
        case 'M':
        case 'L':
        case 'T':
            if (!numberListReadArray(r.data, &r.pos, n, 2))
                return false;
            break;
        case 'H':
        case 'V':
            if (!numberListReadArray(r.data, &r.pos, n, 1))
                return false;
            break;
        case 'C':
            if (!numberListReadArray(r.data, &r.pos, n, 6))
                return false;
            break;
        case 'S':
        case 'Q':
            if (!numberListReadArray(r.data, &r.pos, n, 4))
                return false;
            break;
        case 'A':
            if (!numberListReadArray(r.data, &r.pos, n, 3))
                return false;
            numberListSkipSeparator(r.data, &r.pos);
            if (!readFlag(r.data, &r.pos, &n[3]))
                return false;
            numberListSkipSeparator(r.data, &r.pos);
            if (!readFlag(r.data, &r.pos, &n[4]))
                return false;
            numberListSkipSeparator(r.data, &r.pos);
            if (!numberListReadArray(r.data, &r.pos, n + 5, 2))
                return false;
            break;
        case 'Z':
            break;
        default:
            return false;
    }
    // Pairs after the first pair of a moveto are linetos.
    if (upperOf(r.command) == 'M')
        r.command = r.command == 'm' ? 'l' : 'L';
    *out = command;
    r.failed = false;
    return true;
}

// Returns the angle from `u` to `v`, with the sign that the SVG arc conversion uses.
float angleBetween(PrVec2 u, PrVec2 v) {
    float dot = u.x * v.x + u.y * v.y;
    float length = sqrtF32((u.x * u.x + u.y * u.y) * (v.x * v.x + v.y * v.y));
    float angle = acosF32(clampF32(length > 0 ? dot / length : 1, -1, 1));
    return u.x * v.y - u.y * v.x < 0 ? -angle : angle;
}

// Adds the elliptical arc from `from` to `to` (SVG2/implnote.html#ArcConversionEndpointToCenter). Out-of-range radii
// are scaled up, and zero radii give a straight line (SVG2/implnote.html#ArcCorrectionOutOfRangeRadii).
//
// An arc that ends where it starts is a segment of zero length, whose caps are painted
// (SVG2/paths.html#ZeroLengthSegments), rather than omitted as SVG2/paths.html#ArcOutOfRangeParameters says. The
// omission would leave a lone moveto, which is not stroked.
void arcTo(PrPath* path, PrVec2 from, float rx, float ry, float rotation, bool large, bool sweep,
           PrVec2 to) {
    rx = absF32(rx);
    ry = absF32(ry);
    if ((from.x == to.x && from.y == to.y) || rx == 0 || ry == 0) {
        prPathLineTo(path, to);
        return;
    }
    float phi = degreesToRadiansF32(rotation);
    float c = cosF32(phi);
    float s = sinF32(phi);
    float dx = (from.x - to.x) / 2;
    float dy = (from.y - to.y) / 2;
    float x1 = c * dx + s * dy;
    float y1 = -s * dx + c * dy;
    float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
        float scale = sqrtF32(lambda);
        rx *= scale;
        ry *= scale;
    }
    float numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    float denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float factor = denominator > 0 ? sqrtF32(maxF32(numerator, 0) / denominator) : 0;
    if (large == sweep)
        factor = -factor;
    float cx1 = factor * rx * y1 / ry;
    float cy1 = -factor * ry * x1 / rx;
    PrVec2 center{c * cx1 - s * cy1 + (from.x + to.x) / 2,
                   s * cx1 + c * cy1 + (from.y + to.y) / 2};
    PrVec2 u{(x1 - cx1) / rx, (y1 - cy1) / ry};
    PrVec2 v{(-x1 - cx1) / rx, (-y1 - cy1) / ry};
    float start = angleBetween(PrVec2{1, 0}, u);
    float delta = angleBetween(u, v);
    if (!sweep && delta > 0)
        delta -= 2 * kPiF32;
    else if (sweep && delta < 0)
        delta += 2 * kPiF32;
    prPathEllipticArcTo(path, center, PrVec2{rx, ry}, phi, start, delta);
}

// The state that a command reads from the commands before it.
struct DataPosition {
    PrVec2 current;
    PrVec2 start;               // the start of the current subpath, to which a closepath returns
    PrVec2 lastControl;         // the second control point of the last curve, for S and T
    char previous;              // the uppercase letter of the last command, or 0 before the first
};

// Appends `command` to `path`.
void appendCommand(PrPath* path, DataPosition& p, const PrPathDataCommand& command) {
    bool relative = command.letter >= 'a';
    PrVec2 base = relative ? p.current : PrVec2{0, 0};
    const float* n = command.arguments;
    char upper = upperOf(command.letter);
    switch (upper) {
        case 'M':
            p.current = PrVec2{base.x + n[0], base.y + n[1]};
            p.start = p.current;
            prPathMoveTo(path, p.current);
            break;
        case 'L':
            p.current = PrVec2{base.x + n[0], base.y + n[1]};
            prPathLineTo(path, p.current);
            break;
        case 'H':
            p.current.x = base.x + n[0];
            prPathLineTo(path, p.current);
            break;
        case 'V':
            p.current.y = base.y + n[0];
            prPathLineTo(path, p.current);
            break;
        case 'C': {
            PrVec2 c1{base.x + n[0], base.y + n[1]};
            p.lastControl = PrVec2{base.x + n[2], base.y + n[3]};
            p.current = PrVec2{base.x + n[4], base.y + n[5]};
            prPathCubicTo(path, c1, p.lastControl, p.current);
            break;
        }
        case 'S': {
            bool smooth = p.previous == 'C' || p.previous == 'S';
            PrVec2 c1 = smooth ? PrVec2{2 * p.current.x - p.lastControl.x, 2 * p.current.y - p.lastControl.y}
                               : p.current;
            p.lastControl = PrVec2{base.x + n[0], base.y + n[1]};
            p.current = PrVec2{base.x + n[2], base.y + n[3]};
            prPathCubicTo(path, c1, p.lastControl, p.current);
            break;
        }
        case 'Q':
            p.lastControl = PrVec2{base.x + n[0], base.y + n[1]};
            p.current = PrVec2{base.x + n[2], base.y + n[3]};
            prPathQuadTo(path, p.lastControl, p.current);
            break;
        case 'T': {
            bool smooth = p.previous == 'Q' || p.previous == 'T';
            p.lastControl = smooth ? PrVec2{2 * p.current.x - p.lastControl.x, 2 * p.current.y - p.lastControl.y}
                                   : p.current;
            p.current = PrVec2{base.x + n[0], base.y + n[1]};
            prPathQuadTo(path, p.lastControl, p.current);
            break;
        }
        case 'A': {
            PrVec2 to{base.x + n[5], base.y + n[6]};
            arcTo(path, p.current, n[0], n[1], n[2], n[3] > 0, n[4] > 0, to);
            p.current = to;
            break;
        }
        case 'Z':
            prPathClose(path);
            p.current = p.start;
            break;
    }
    p.previous = upper;
}

// Returns `v` at unit length, or zero if it has no length.
PrVec2 unitOf(PrVec2 v) {
    float length = sqrtF32(v.x * v.x + v.y * v.y);
    return length > 0 ? PrVec2{v.x / length, v.y / length} : PrVec2{0, 0};
}

// How far apart, as a fraction of a segment's size, two of its points are before they give it a direction. The curves
// that an arc appends begin at the arc's start as computed, which may lie a rounding error from the current point.
constexpr float kDirectionTolerance = 1e-5f;

// Writes the directions of the segment that `points` describe, its start point first: out of the start toward the first
// point that differs from it, and into the end from the last point that differs from the end. Both are zero for a
// segment whose points are all the same, which has no length.
void directionsOf(const PrVec2* points, uint32_t count, PrPathDataVertex* out) {
    out->startDirection = out->endDirection = PrVec2{0, 0};
    if (count == 0)
        return;
    PrVec2 low = points[0];
    PrVec2 high = points[0];
    for (uint32_t i = 1; i < count; ++i) {
        low = PrVec2{minF32(low.x, points[i].x), minF32(low.y, points[i].y)};
        high = PrVec2{maxF32(high.x, points[i].x), maxF32(high.y, points[i].y)};
    }
    float size = maxF32(high.x - low.x, high.y - low.y);
    if (!(size > 0))
        return;
    float tolerance = size * kDirectionTolerance;
    auto apart = [&](PrVec2 a, PrVec2 b) { return absF32(a.x - b.x) > tolerance || absF32(a.y - b.y) > tolerance; };
    for (uint32_t i = 1; i < count; ++i) {
        if (apart(points[i], points[0])) {
            out->startDirection = unitOf(points[i] - points[0]);
            break;
        }
    }
    PrVec2 end = points[count - 1];
    for (uint32_t i = count - 1; i-- > 0;) {
        if (apart(points[i], end)) {
            out->endDirection = unitOf(end - points[i]);
            break;
        }
    }
}

bool hasLength(const PrPathDataVertex& vertex) {
    return vertex.startDirection.x != 0 || vertex.startDirection.y != 0;
}

// Appends `value` to `out` with the fewest significant digits that parse back to it.
void appendNumber(PrString* out, float value) {
    char text[kF64TextMaxBytes];
    size_t length = 0;
    for (uint32_t digits = 6; digits <= 9; ++digits) {
        length = formatF64Significant(double(value), digits, text);
        double parsed = 0;
        if (decimalToF64(PrStringView{text, length}, &parsed) && float(parsed) == value)
            break;
    }
    prStringAppend(out, PrStringView{text, length});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prPathAppendData(PrPath* path, PrStringView data) {
    DataReader reader{data, 0, 0, false};
    DataPosition position{};
    PrPathDataCommand command;
    while (readCommand(reader, &command))
        appendCommand(path, position, command);
    return reader.failed ? PrStatus_Syntax : PrStatus_Ok;
}

uint32_t prPathDataArgumentCount(char letter) {
    switch (upperOf(letter)) {
        case 'M':
        case 'L':
        case 'T':
            return 2;
        case 'H':
        case 'V':
            return 1;
        case 'C':
            return 6;
        case 'S':
        case 'Q':
            return 4;
        case 'A':
            return 7;
        case 'Z':
            return 0;
        default:
            PR_ASSERT(false);
            return 0;
    }
}

uint32_t prPathDataParse(PrStringView data, PrPathDataCommand* PR_NULLABLE out, uint32_t capacity) {
    DataReader reader{data, 0, 0, false};
    PrPathDataCommand command;
    uint32_t count = 0;
    while (readCommand(reader, &command)) {
        if (count < capacity)
            out[count] = command;
        ++count;
    }
    return count;
}

void prPathAppendCommands(PrPath* path, const PrPathDataCommand* PR_NULLABLE commands, uint32_t count) {
    DataPosition position{};
    for (uint32_t i = 0; i < count; ++i)
        appendCommand(path, position, commands[i]);
}

void prPathDataVertices(const PrPathDataCommand* PR_NULLABLE commands, uint32_t count,
                        PrPathDataVertex* PR_NULLABLE out) {
    // Each segment's own directions, from the curves that its command appends after the current point.
    PrPath* segment = prPathCreate();
    DataPosition position{};
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) {
        char upper = upperOf(commands[i].letter);
        PrVec2 from = position.current;
        PrVec2 start = position.start;
        prPathClear(segment);
        prPathMoveTo(segment, from);
        appendCommand(segment, position, commands[i]);
        out[i].point = position.current;
        if (upper == 'M') {
            out[i].startDirection = out[i].endDirection = PrVec2{0, 0};
        } else if (upper == 'Z') {
            PrVec2 line[2] = {from, start};
            directionsOf(line, 2, &out[i]);
        } else {
            directionsOf(prPathPoints(segment), prPathPointCount(segment), &out[i]);
        }
        any = any || hasLength(out[i]);
    }
    prPathDestroy(segment);
    if (!any) {
        for (uint32_t i = 0; i < count; ++i)
            out[i].startDirection = out[i].endDirection = PrVec2{1, 0};
        return;
    }
    // A segment of zero length starts in the direction that the closest segment before it with length ends in, and ends
    // in the direction that the closest one after it starts in. Without one on a side, its direction at that end is its
    // direction at the other. The first pass sets only the start of a segment of zero length, so its end is still zero
    // in the second.
    PrVec2 before{0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        if (hasLength(out[i]))
            before = out[i].endDirection;
        else
            out[i].startDirection = before;
    }
    PrVec2 after{0, 0};
    for (uint32_t i = count; i-- > 0;) {
        if (out[i].endDirection.x != 0 || out[i].endDirection.y != 0)
            after = out[i].startDirection;
        else
            out[i].endDirection = after;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PrPathDataVertex& v = out[i];
        if (v.startDirection.x == 0 && v.startDirection.y == 0)
            v.startDirection = v.endDirection;
        if (v.endDirection.x == 0 && v.endDirection.y == 0)
            v.endDirection = v.startDirection;
    }
}

void prPathDataMakeAbsolute(PrPathDataCommand* PR_NULLABLE commands, uint32_t count) {
    PrVec2 current{0, 0};
    PrVec2 start{0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        PrPathDataCommand& command = commands[i];
        float* n = command.arguments;
        char upper = upperOf(command.letter);
        // The arguments that are x and y coordinates, as the indices of the first of each pair.
        uint32_t pairs[3] = {};
        uint32_t pairCount = 0;
        switch (upper) {
            case 'M':
            case 'L':
            case 'T':
                pairs[pairCount++] = 0;
                break;
            case 'C':
                pairs[pairCount++] = 0;
                pairs[pairCount++] = 2;
                pairs[pairCount++] = 4;
                break;
            case 'S':
            case 'Q':
                pairs[pairCount++] = 0;
                pairs[pairCount++] = 2;
                break;
            case 'A':
                pairs[pairCount++] = 5;
                break;
        }
        if (command.letter != upper) {
            for (uint32_t k = 0; k < pairCount; ++k) {
                n[pairs[k]] += current.x;
                n[pairs[k] + 1] += current.y;
            }
            if (upper == 'H')
                n[0] += current.x;
            else if (upper == 'V')
                n[0] += current.y;
            command.letter = upper;
        }
        if (pairCount != 0)
            current = PrVec2{n[pairs[pairCount - 1]], n[pairs[pairCount - 1] + 1]};
        else if (upper == 'H')
            current.x = n[0];
        else if (upper == 'V')
            current.y = n[0];
        else if (upper == 'Z')
            current = start;
        if (upper == 'M')
            start = current;
    }
}

void prPathDataWrite(const PrPathDataCommand* PR_NULLABLE commands, uint32_t count, PrString* out) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrPathDataCommand& command = commands[i];
        if (i != 0)
            prStringAppend(out, " ");
        // A closepath has no coordinates, so z and Z are the same command.
        prStringAppend(out, upperOf(command.letter) == 'Z' ? PrStringView{"Z"} : PrStringView{&command.letter, 1});
        bool arc = upperOf(command.letter) == 'A';
        uint32_t arguments = prPathDataArgumentCount(command.letter);
        for (uint32_t k = 0; k < arguments; ++k) {
            prStringAppend(out, " ");
            if (arc && (k == 3 || k == 4))
                prStringAppend(out, command.arguments[k] > 0 ? "1" : "0");
            else
                appendNumber(out, command.arguments[k]);
        }
    }
}
