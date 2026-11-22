// The harness of the prax-paths suite (tools/suite/prax/paths.py): the path library's fills and combinations, over the
// shapes of suites/prax-paths.
//
// `path_check <path>...` checks the shape in each file and writes a line of JSON for each, {"ok": <bool>, "detail":
// <text>}, whose detail says what went wrong when the check fails. The file's directory names its check:
//
//   polygons   the tessellator's triangles cover each point of a grid once where the fill rule fills it and not at all
//              elsewhere, by both rules; and with a fringe, so wherever the point lies away from the boundary
//   curves     the sparse rasterizer's spans of each kind, cut to the file's box, cover each pixel at most once, and
//              as the rasterizer covers it, by both rules
//   pairs      the two shapes combined by each operation, by both rules, wind once around what they cover, and cover
//              each point of a grid as the shapes' windings say they do

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/blob_ref.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/min_max.h"
#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/path_rasterizer.h"
#include "pr/path_sparse.h"
#include "pr/path_tessellator.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

using namespace harness;
using namespace pr;

namespace {

constexpr PrPathFillRule kRules[] = {PrPathFillRule_NonZero, PrPathFillRule_EvenOdd};

const char* ruleName(PrPathFillRule rule) {
    return rule == PrPathFillRule_NonZero ? "nonzero" : "evenodd";
}

// A file's shapes, and the box that a fill of curves is cut to.
struct Shapes {
    Array<PrPath*> paths;
    PrIRect box{};
    String error;

    ~Shapes() {
        for (PrPath* path : paths)
            prPathDestroy(path);
    }
};

// Appends the words of `line`, which spaces separate, to `out`.
void words(PrStringView line, Array<PrStringView>* out) {
    size_t at = 0;
    while (at < line.size) {
        while (at < line.size && line.data[at] == ' ')
            ++at;
        size_t start = at;
        while (at < line.size && line.data[at] != ' ')
            ++at;
        if (at > start)
            out->push(PrStringView{line.data + start, at - start});
    }
}

float numberOf(PrStringView word) {
    return float(prF64Parse(word.data, word.size));
}

Shapes read(PrStringView file) {
    Shapes shapes;
    BlobRef blob = BlobRef::adopt(prFileOpen(file));
    if (blob.get() == nullptr) {
        shapes.error = "the file cannot be read";
        return shapes;
    }
    PrByteView bytes = prBlobBytes(blob.get());
    PrStringView text{reinterpret_cast<const char*>(bytes.data), bytes.size};
    PrPath* path = nullptr;
    size_t at = 0;
    while (at <= text.size) {
        PrStringView rest = prStringFrom(text, at);
        size_t end = at + prStringFindByte(rest, '\n');
        PrStringView line = prStringHead(rest, end - at);
        at = end + 1;
        Array<PrStringView> w;
        words(line, &w);
        if (w.isEmpty()) {
            path = nullptr;
            if (end == text.size)
                break;
            continue;
        }
        if (w[0] == "box" && w.count() == 5) {
            shapes.box = PrIRect{
                int32_t(numberOf(w[1])), int32_t(numberOf(w[2])),
                int32_t(numberOf(w[1]) + numberOf(w[3])), int32_t(numberOf(w[2]) + numberOf(w[4])),
            };
            continue;
        }
        if (path == nullptr) {
            path = prPathCreate();
            shapes.paths.push(path);
        }
        for (uint32_t i = 0; i < w.count();) {
            PrStringView verb = w[i++];
            auto point = [&] {
                PrVec2 p{numberOf(w[i]), numberOf(w[i + 1])};
                i += 2;
                return p;
            };
            if (verb == "M" && i + 2 <= w.count()) {
                prPathMoveTo(path, point());
            } else if (verb == "L" && i + 2 <= w.count()) {
                prPathLineTo(path, point());
            } else if (verb == "Q" && i + 4 <= w.count()) {
                PrVec2 control = point();
                prPathQuadTo(path, control, point());
            } else if (verb == "Z") {
                prPathClose(path);
            } else {
                shapes.error = format("the path data has no verb {}", verb);
                return shapes;
            }
        }
        if (end == text.size)
            break;
    }
    return shapes;
}

double twiceArea(PrVec2 a, PrVec2 b, PrVec2 c) {
    return (double(b.x) - double(a.x)) * (double(c.y) - double(a.y)) -
        (double(b.y) - double(a.y)) * (double(c.x) - double(a.x));
}

// Calls `each` with the ends of each edge of a path of straight contours.
template <typename Each>
void edges(const PrPath* path, Each each) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t at = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        if (verbs[i] == PrPathVerb_Move) {
            start = at++;
        } else if (verbs[i] == PrPathVerb_Line) {
            ++at;
        } else if (verbs[i] == PrPathVerb_Close) {
            for (uint32_t k = start; k < at; ++k)
                each(points[k], points[k + 1 < at ? k + 1 : start]);
        }
    }
}

// The winding of a path of straight contours around a point, counted by the crossings of a ray toward +x.
int winding(const PrPath* path, PrVec2 p) {
    int w = 0;
    edges(path, [&](PrVec2 a, PrVec2 b) {
        if ((a.y <= p.y) == (b.y <= p.y))
            return;
        float x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
        if (x > p.x)
            w += b.y > a.y ? 1 : -1;
    });
    return w;
}

bool fills(PrPathFillRule rule, int w) {
    return rule == PrPathFillRule_EvenOdd ? (w & 1) != 0 : w != 0;
}

// The distance from the point to the nearest edge of a path of straight contours.
double nearestEdge(const PrPath* path, PrVec2 p) {
    double nearest = 1e9;
    edges(path, [&](PrVec2 a, PrVec2 b) {
        double dx = double(b.x) - double(a.x);
        double dy = double(b.y) - double(a.y);
        double length2 = dx * dx + dy * dy;
        double u = length2 > 0 ? ((double(p.x) - double(a.x)) * dx + (double(p.y) - double(a.y)) * dy) / length2 : 0;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        double ex = double(a.x) + u * dx - double(p.x);
        double ey = double(a.y) + u * dy - double(p.y);
        nearest = prF64Min(nearest, prF64Sqrt(ex * ex + ey * ey));
    });
    return nearest;
}

// The number of triangles of the mesh that contain the point.
uint32_t containing(const PrPathMesh& mesh, PrVec2 p) {
    uint32_t count = 0;
    for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3) {
        PrVec2 a = mesh.points[mesh.indices[i]];
        PrVec2 b = mesh.points[mesh.indices[i + 1]];
        PrVec2 c = mesh.points[mesh.indices[i + 2]];
        double d1 = twiceArea(a, b, p);
        double d2 = twiceArea(b, c, p);
        double d3 = twiceArea(c, a, p);
        if ((d1 > 0 && d2 > 0 && d3 > 0) || (d1 < 0 && d2 < 0 && d3 < 0))
            ++count;
    }
    return count;
}

String polygons(const PrPath* path) {
    PrPathTessellator* t = prPathTessellatorCreate();
    PrPathPolyline polyline{};
    prPathFlatten(path, prAffine2Identity(), 0.1f, PrPathFlattening_Balanced, &polyline);
    String detail;
    for (PrPathFillRule rule : kRules) {
        PrPathMesh mesh = prPathTessellatorFill(t, &polyline, rule, 0);
        uint32_t wrong = 0;
        // Points off a grid of 20 and its diagonals, where the snapped shapes put their vertices and edges.
        for (int sy = 0; sy < 40; ++sy) {
            for (int sx = 0; sx < 40; ++sx) {
                PrVec2 p{float(sx) * 2.5f + 0.3173f, float(sy) * 2.5f + 0.7331f};
                wrong += containing(mesh, p) != (fills(rule, winding(path, p)) ? 1u : 0u) ? 1 : 0;
            }
        }
        if (wrong != 0 && detail.isEmpty())
            detail = format("{}: {} points covered otherwise", ruleName(rule), wrong);
    }
    // With a fringe, the points away from the boundary.
    PrPathMesh mesh = prPathTessellatorFill(t, &polyline, PrPathFillRule_NonZero, 1);
    uint32_t wrong = 0;
    for (int sy = 0; sy < 50; ++sy) {
        for (int sx = 0; sx < 50; ++sx) {
            PrVec2 p{float(sx) * 2 + 0.3173f, float(sy) * 2 + 0.7331f};
            if (nearestEdge(path, p) < 1.5)
                continue;
            wrong += containing(mesh, p) != (winding(path, p) != 0 ? 1u : 0u) ? 1 : 0;
        }
    }
    if (wrong != 0 && detail.isEmpty())
        detail = format("with a fringe: {} points away from the boundary covered otherwise", wrong);
    prPathPolylineReset(&polyline);
    prPathTessellatorDestroy(t);
    return detail;
}

float coverageOfWinding(float winding, PrPathFillRule rule) {
    float magnitude = prF32Abs(winding);
    if (rule == PrPathFillRule_EvenOdd) {
        magnitude -= 2 * prF32Floor(magnitude / 2);
        return magnitude > 1 ? 2 - magnitude : magnitude;
    }
    return prF32Min(magnitude, 1.0f);
}

// The integral of clamp(u, 0, 1) from 0 to u.
float integral(float u) {
    if (u <= 0)
        return 0;
    return u < 1 ? u * u / 2 : u - 0.5f;
}

// The winding of a pixel in a tile record, as pr/path_sparse.h defines it.
float tileWinding(const float* record, uint32_t height, float px, uint32_t row) {
    uint32_t count = uint32_t(record[0]);
    float winding = record[1 + row];
    const float* segments = record + 1 + height;
    for (uint32_t i = 0; i < count; ++i) {
        const float* s = segments + i * 4;
        float top = prF32Min(s[1], s[3]);
        float bottom = prF32Max(s[1], s[3]);
        float y0 = prF32Max(top, float(row));
        float y1 = prF32Min(bottom, float(row + 1));
        if (!(y1 > y0))
            continue;
        auto xAt = [&](float y) { return s[0] + (s[2] - s[0]) * (y - s[1]) / (s[3] - s[1]); };
        float xa = xAt(y0);
        float xb = xAt(y1);
        float average;
        if (prF32Abs(xb - xa) < 1e-6f)
            average = prF32Min(prF32Max(px + 1 - (xa + xb) / 2, 0.0f), 1.0f);
        else
            average = (integral(px + 1 - xa) - integral(px + 1 - xb)) / (xb - xa);
        winding += (s[3] > s[1] ? 1 : -1) * (y1 - y0) * average;
    }
    return winding;
}

// Renders the spans into the pixels of the box. Returns what went wrong: a span outside the box, or a pixel that two
// spans cover.
String render(const PrPathSparse& sparse, const PrPathSparseRasterizerOptions& options, PrPathFillRule rule,
        PrIRect box, Array<uint8_t>* pixels) {
    uint32_t width = uint32_t(box.right - box.left);
    uint32_t height = uint32_t(box.bottom - box.top);
    Array<uint8_t> covered(size_t(width) * height);
    for (uint32_t i = 0; i < sparse.spanCount; ++i) {
        const PrPathSparseSpan& s = sparse.spans[i];
        if (s.origin.x < box.left || s.origin.y < box.top || s.origin.x + int32_t(s.extent.x) > box.right ||
                s.origin.y + int32_t(s.extent.y) > box.bottom)
            return format("span {} lies outside the box", i);
        for (uint32_t y = 0; y < s.extent.y; ++y) {
            for (uint32_t x = 0; x < s.extent.x; ++x) {
                uint32_t row = uint32_t(s.origin.y - box.top + int32_t(y));
                uint32_t at = row * width + uint32_t(s.origin.x - box.left + int32_t(x));
                if (covered[at] != 0)
                    return format("span {} covers a pixel that another covers", i);
                covered[at] = 1;
                uint8_t value = 255;
                if (s.data != kPrPathSparseSolid && options.kind == PrPathSparseKind_Coverage) {
                    value = sparse.coverage[s.data + y * s.extent.x + x];
                } else if (s.data != kPrPathSparseSolid) {
                    uint32_t tile = x / options.tileExtent.x;
                    const float* record = sparse.tileData + s.data;
                    for (uint32_t k = 0; k < tile; ++k)
                        record += 1 + s.extent.y + uint32_t(record[0]) * 4;
                    float px = float(x - tile * options.tileExtent.x);
                    value = uint8_t(coverageOfWinding(tileWinding(record, s.extent.y, px, y), rule) * 255 + 0.5f);
                }
                (*pixels)[at] = value;
            }
        }
    }
    return {};
}

String curves(const PrPath* path, PrIRect box) {
    const PrPathSparseRasterizerOptions kinds[] = {
        {PrPathSparseKind_Coverage, {4, 4}},
        {PrPathSparseKind_Segments, {16, 16}},
        {PrPathSparseKind_Segments, {8, 4}},
        {PrPathSparseKind_Coverage, {16, 2}},
    };
    uint32_t width = uint32_t(box.right - box.left);
    uint32_t height = uint32_t(box.bottom - box.top);
    PrPathPolyline polyline{};
    prPathFlatten(path, prAffine2Identity(), 0.05f, PrPathFlattening_Balanced, &polyline);
    PrPathRasterizer* reference = prPathRasterizerCreate();
    String detail;
    for (PrPathFillRule rule : kRules) {
        Array<uint8_t> expected(size_t(width) * height);
        PrPixels mask{expected.data(), width, {width, height}};
        prPathRasterizerFill(reference, &polyline, rule, PrPathCoverage_Area, PrIVec2{box.left, box.top}, mask);
        for (const PrPathSparseRasterizerOptions& options : kinds) {
            PrPathSparseRasterizer* rasterizer = prPathSparseRasterizerCreate(&options);
            PrPathSparse sparse = prPathSparseRasterizerFill(rasterizer, &polyline, rule, box);
            Array<uint8_t> drawn(size_t(width) * height);
            String wrong = render(sparse, options, rule, box, &drawn);
            int largest = 0;
            for (uint32_t i = 0; wrong.isEmpty() && i < drawn.count(); ++i) {
                int difference = int(drawn[i]) - int(expected[i]);
                largest = max(largest, difference < 0 ? -difference : difference);
            }
            if (wrong.isEmpty() && largest > 2)
                wrong = format("a pixel differs by {} from the rasterizer's", largest);
            if (!wrong.isEmpty() && detail.isEmpty()) {
                detail = format("{}, {} tiles of {}x{}: {}", ruleName(rule),
                    options.kind == PrPathSparseKind_Coverage ? "coverage" : "segment", options.tileExtent.x,
                    options.tileExtent.y, wrong);
            }
            prPathSparseRasterizerDestroy(rasterizer);
        }
    }
    prPathRasterizerDestroy(reference);
    prPathPolylineReset(&polyline);
    return detail;
}

// Returns true if the combination `op` covers a point that the first region covers if `ia` and the second if `ib`.
bool combines(PrPathPolylineOp op, bool ia, bool ib) {
    if (op == PrPathPolylineOp_Union)
        return ia || ib;
    if (op == PrPathPolylineOp_Intersect)
        return ia && ib;
    if (op == PrPathPolylineOp_Difference)
        return ia && !ib;
    return ia != ib;
}

String pairs(const PrPath* a, const PrPath* b) {
    const PrPathPolylineOp ops[] = {
        PrPathPolylineOp_Union, PrPathPolylineOp_Intersect, PrPathPolylineOp_Difference, PrPathPolylineOp_Xor,
    };
    const char* const names[] = {"union", "intersect", "difference", "xor"};
    PrPathPolyline flatA{};
    PrPathPolyline flatB{};
    prPathFlatten(a, prAffine2Identity(), 0.01f, PrPathFlattening_Inside, &flatA);
    prPathFlatten(b, prAffine2Identity(), 0.01f, PrPathFlattening_Inside, &flatB);
    String detail;
    for (PrPathFillRule rule : kRules) {
        for (uint32_t o = 0; o < countOf(ops); ++o) {
            PrPathPolyline result{};
            prPathPolylineCombine(&flatA, rule, &flatB, rule, ops[o], &result);
            PrPath* out = prPathCreate();
            prPathAddPolyline(out, &result, prAffine2Identity());
            uint32_t wrong = 0;
            uint32_t windings = 0;
            for (uint32_t y = 0; y < 50; ++y) {
                for (uint32_t x = 0; x < 50; ++x) {
                    PrVec2 p{float(x) * 2 + 0.37f, float(y) * 2 + 0.61f};
                    bool ia = fills(rule, winding(a, p));
                    bool ib = fills(rule, winding(b, p));
                    bool expected = combines(ops[o], ia, ib);
                    int w = winding(out, p);
                    windings += w < -1 || w > 1 ? 1 : 0;
                    wrong += (w != 0) != expected ? 1 : 0;
                }
            }
            if ((wrong != 0 || windings != 0) && detail.isEmpty()) {
                detail = format("{} by {}: {} points covered otherwise, {} wound more than once", names[o],
                    ruleName(rule), wrong, windings);
            }
            prPathDestroy(out);
            prPathPolylineReset(&result);
        }
    }
    prPathPolylineReset(&flatA);
    prPathPolylineReset(&flatB);
    return detail;
}

String check(PrStringView file) {
    Shapes shapes = read(file);
    if (!shapes.error.isEmpty())
        return shapes.error;
    if (prStringContains(file, "/polygons/") && shapes.paths.count() == 1)
        return polygons(shapes.paths[0]);
    if (prStringContains(file, "/curves/") && shapes.paths.count() == 1)
        return curves(shapes.paths[0], shapes.box);
    if (prStringContains(file, "/pairs/") && shapes.paths.count() == 2)
        return pairs(shapes.paths[0], shapes.paths[1]);
    return "the file is not a shape of its directory";
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        String detail = check(argv[i]);
        PrTextBuffer line{};
        prTextBufferAppend(&line, detail.isEmpty() ? "{\"ok\": true, \"detail\": " : "{\"ok\": false, \"detail\": ");
        harnessWriteString(&line, detail);
        prTextBufferAppend(&line, "}");
        harnessWriteLine(prTextBufferView(&line));
        prTextBufferReset(&line);
    }
    return 0;
}
