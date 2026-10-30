// Feature drawings for shapes and strokes.

#include "tools/viewer/features_shapes.h"

#include "pr/path_polyline.h"
#include "pr/vec.h"

#include "tools/viewer/drawing.h"

#include <cstdio>
#include <string>

namespace viewer {
namespace {

void fills(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    PrRect c = grid.cell(0, 0);
    prDrawListRect(list, c, {.color = kBlue});
    c = grid.cell(1, 0);
    prDrawListCornerRect(list, prCornerRectRound(c, 24), {.color = kGreen});
    c = grid.cell(2, 0);
    PrCornerRect mixedRadii{
        c, {prCornerRound(PrVec2{60, 20}), prCornerRound(PrVec2{8, 8}), {}, prCornerRound(PrVec2{30, 90})},
    };
    prDrawListCornerRect(list, mixedRadii, {.color = kGold});
    c = grid.cell(3, 0);
    prDrawListCornerRect(list, prCornerRectRound(c, 400), {.color = kRed});
    c = grid.cell(0, 1);
    prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) / 2, extentOf(c) / 2}, 0, {.color = kBlue});
    c = grid.cell(1, 1);
    prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) / 2, extentOf(c) / 4}, kPi / 6, {.color = kGreen});
    c = grid.cell(2, 1);
    PrVec2 triangle[] = {{c.left, c.bottom}, {(c.left + c.right) / 2, c.top}, {c.right, c.bottom}};
    prDrawListPolylineContour(list, triangle, 3, PrDrawContour_Closed, PrPathFillRule_NonZero, {.color = kGold});
    c = grid.cell(3, 1);
    PrVec2 center = prRectCenter(c);
    float r = extentOf(c) / 3;
    prDrawListEllipse(list, PrVec2{center.x - r / 2, center.y}, PrVec2{r, r}, 0, {.color = withAlpha(kRed, 0.6f)});
    prDrawListEllipse(list, PrVec2{center.x + r / 2, center.y}, PrVec2{r, r}, 0, {.color = withAlpha(kBlue, 0.6f)});
    const char* captions[] = {
        "rect",   "rounded rect",    "per-corner radii",
        "oversized radii, fitted", "circle", "rotated ellipse",
        "polygon", "translucent overlap",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i]);
}

void fillRules(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    for (uint32_t row = 0; row < 2; ++row) {
        PrPathFillRule rule = row == 0 ? PrPathFillRule_NonZero : PrPathFillRule_EvenOdd;
        PrRect c = grid.cell(0, row);
        PrPath* path = star(prRectCenter(c), extentOf(c) / 2, 5, 2);
        prDrawListPath(list, path, rule, {.color = kBlue});
        prPathDestroy(path);

        c = grid.cell(1, row);
        PrVec2 center = prRectCenter(c);
        float r = extentOf(c) / 2;
        path = prPathCreate();
        prPathAddEllipse(path, center, PrVec2{r, r}, 0);
        prPathAddEllipse(path, center, PrVec2{r / 2, r / 2}, 0);
        prDrawListPath(list, path, rule, {.color = kGreen});
        prPathDestroy(path);

        c = grid.cell(2, row);
        center = prRectCenter(c);
        r = extentOf(c) / 3;
        path = prPathCreate();
        prPathAddEllipse(path, PrVec2{center.x - r * 0.5f, center.y}, PrVec2{r, r}, 0);
        prPathAddEllipse(path, PrVec2{center.x + r * 0.5f, center.y}, PrVec2{r, r}, 0);
        prDrawListPath(list, path, rule, {.color = kGold});
        prPathDestroy(path);
        const char* name = row == 0 ? "nonzero" : "evenodd";
        const char* shapes[] = {"pentagram", "nested circles", "overlapping circles"};
        for (uint32_t column = 0; column < 3; ++column) {
            std::string text = std::string(shapes[column]) + ", " + name;
            grid.caption(column, row, text.c_str());
        }
    }
}

void corners(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 5, 3};
    const float shapes[] = {-INFINITY, -2, -1, -0.5f, 0, 0.5f, 1, 2, 4, INFINITY};
    for (uint32_t i = 0; i < 10; ++i) {
        PrRect c = squareIn(grid.at(i, 20), 150);
        PrCorner k = cornerOf(50, shapes[i]);
        prDrawListCornerRect(list, PrCornerRect{c, {k, k, k, k}}, {.color = i < 4 ? kPurple : i < 5 ? kGold : kBlue});
        const char* names[] = {
            "K -inf, notch", "K -2", "K -1, scoop", "K -0.5",
            "K 0, bevel",    "K 0.5", "K 1, round", "K 2, squircle",
            "K 4",           "K +inf, square",
        };
        grid.captionAt(i, names[i], 20);
    }
    // Mixed shapes and radii, filled and stroked, and a stroke's outline through a scoop.
    PrRect c = squareIn(grid.at(10, 20), 150);
    PrCornerRect mixed{c, {cornerOf(60, 2), cornerOf(20, -1), prCornerRound(PrVec2{70, 30}), cornerOf(40, 0)}};
    prDrawListCornerRect(list, mixed, {.color = kTeal});
    mixed.rect = squareIn(grid.at(11, 20), 150);
    PrDrawStroke stroke = strokeOf(8, PrPathJoin_Round);
    prDrawListCornerRect(list, mixed, {.color = kInk, .stroke = &stroke});
    c = squareIn(grid.at(12, 20), 150);
    PrCornerRect scoops{c, {cornerOf(90, -1), cornerOf(90, -1), cornerOf(90, -1), cornerOf(90, -1)}};
    prDrawListCornerRect(list, scoops, {.color = kRed});
    c = squareIn(grid.at(13, 20), 150);
    PrCornerRect squircles{c, {cornerOf(75, 2), cornerOf(75, 2), cornerOf(75, 2), cornerOf(75, 2)}};
    prDrawListCornerRect(list, squircles, {.color = kGreen});
    squircles.rect = squareIn(grid.at(14, 20), 150);
    PrDrawStroke dashed = strokeOf(6, PrPathJoin_Round, PrPathCap_Round);
    const float pattern[] = {14, 10};
    dashed.shape.dashes = pattern;
    dashed.shape.dashCount = 2;
    prDrawListCornerRect(list, squircles, {.color = kBlue, .stroke = &dashed});
    const char* more[] = {
        "mixed corners", "mixed corners stroked", "oversized scoops, fitted",
        "squircles", "dashed squircles",
    };
    for (uint32_t i = 0; i < 5; ++i)
        grid.captionAt(10 + i, more[i], 20);
}

void arcsAndCapsules(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    PrRect c = grid.at(0, 30);
    PrVec2 center = prRectCenter(c);
    float r = extentOf(c) / 2;
    prDrawListSector(list, center, 0, r, -kPi / 2, kPi * 1.5f, {.color = kBlue});
    c = grid.at(1, 30);
    center = prRectCenter(c);
    prDrawListSector(list, center, r * 0.55f, r, kPi * 0.75f, kPi * 1.5f, {.color = kGreen});
    c = grid.at(2, 30);
    center = prRectCenter(c);
    prDrawListSector(list, center, r * 0.7f, r, 0, 2 * kPi, {.color = kGold});
    c = grid.at(3, 30);
    center = prRectCenter(c);
    PrDrawStroke outline = strokeOf(6, PrPathJoin_Round);
    prDrawListSector(list, center, r * 0.4f, r, -kPi / 3, -kPi, {.color = kRed, .stroke = &outline});
    for (uint32_t i = 0; i < 6; ++i) {
        float angle = -kPi / 2 + kPi / 5 * float(i);
        prDrawListSector(list, center, r * 0.15f, r * 0.3f, angle, kPi / 8, {.color = kPurple});
    }

    c = grid.at(4, 30);
    prDrawListCapsule(list, PrVec2{c.left + 20, c.top + 40}, PrVec2{c.right - 20, c.bottom - 40}, 30, {.color = kTeal});
    c = grid.at(5, 30);
    for (uint32_t i = 0; i < 5; ++i) {
        float y = c.top + 20 + float(i) * 45;
        prDrawListCapsule(list, PrVec2{c.left + 20, y}, PrVec2{c.right - 20 - float(i) * 40, y}, 4 + float(i) * 3,
            {.color = withAlpha(kBlue, 0.4f + 0.12f * float(i))});
    }
    c = grid.at(6, 30);
    center = prRectCenter(c);
    prDrawListCapsule(list, center, center, 60, {.color = kGold});
    c = grid.at(7, 30);
    PrDrawStroke thin = strokeOf(4);
    prDrawListCapsule(list, PrVec2{c.left + 30, c.bottom - 30}, PrVec2{c.right - 30, c.top + 30}, 36,
        {.color = kRed, .stroke = &thin});
    const char* captions[] = {
        "pie slice",        "annular sector",    "full ring",
        "stroked sector, small sectors", "capsule",
        "capsules of growing radius", "zero length: a circle",
        "stroked capsule",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i], 30);
}

void joinsAndCaps(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 5};
    PrPathJoin joins[] = {PrPathJoin_Miter, PrPathJoin_MiterClip, PrPathJoin_Round, PrPathJoin_Bevel, PrPathJoin_Arcs};
    PrPathCap caps[] = {PrPathCap_Butt, PrPathCap_Round, PrPathCap_Square, PrPathCap_Triangle};
    PrDrawStroke guide = hairlineStroke();
    for (uint32_t row = 0; row < 5; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            PrRect c = grid.cell(column, row, 24);
            float w = prRectWidth(c);
            float h = prRectHeight(c);
            PrVec2 points[] = {
                {c.left, c.bottom},
                {c.left + w * 0.3f, c.top},
                {c.left + w * 0.5f, c.bottom - h * 0.1f},
                {c.right, c.top + h * 0.3f},
            };
            PrDrawStroke stroke = strokeOf(16, joins[row], caps[column]);
            stroke.shape.miterLimit = 2;
            prDrawListPolylineContour(list, points, 4, PrDrawContour_Open, PrPathFillRule_NonZero,
                {.color = withAlpha(kBlue, 0.85f), .stroke = &stroke});
            prDrawListPolylineContour(list, points, 4, PrDrawContour_Open, PrPathFillRule_NonZero,
                {.color = kRed, .stroke = &guide});
            const char* joinNames[] = {"miter", "miter-clip", "round", "bevel", "arcs"};
            const char* capNames[] = {"butt", "round", "square", "triangle"};
            std::string text = std::string(joinNames[row]) + " join, " + capNames[column] + " cap";
            grid.caption(column, row, text.c_str(), 24);
        }
    }
}

void miterLimits(PrDrawList* list, float width, float height, double) {
    // Rows are miter, miter-clip and arcs. Columns sharpen the turn. The limit is 3 throughout.
    Grid grid{width, height, 5, 4};
    PrPathJoin joins[] = {PrPathJoin_Miter, PrPathJoin_MiterClip, PrPathJoin_Arcs};
    float angles[] = {100, 60, 35, 20, 10};
    PrDrawStroke guide = hairlineStroke();
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 5; ++column) {
            PrRect c = grid.cell(column, row, 20);
            PrVec2 tip{c.right - 30, (c.top + c.bottom) / 2};
            float a = angles[column] * kPi / 180 / 2;
            float length = std::fmin(prRectWidth(c) * 0.8f, (prRectHeight(c) / 2 - 8) / std::sin(a));
            PrVec2 points[] = {
                {tip.x - length * std::cos(a), tip.y - length * std::sin(a)},
                tip,
                {tip.x - length * std::cos(a), tip.y + length * std::sin(a)},
            };
            PrDrawStroke stroke = strokeOf(14, joins[row]);
            stroke.shape.miterLimit = 3;
            prDrawListPolylineContour(list, points, 3, PrDrawContour_Open, PrPathFillRule_NonZero,
                {.color = withAlpha(kBlue, 0.85f), .stroke = &stroke});
            prDrawListPolylineContour(list, points, 3, PrDrawContour_Open, PrPathFillRule_NonZero,
                {.color = kRed, .stroke = &guide});
            const char* joinNames[] = {"miter", "miter-clip", "arcs"};
            std::string text = std::string(joinNames[row]) + ", " + std::to_string(int(angles[column])) + " degrees, limit 3";
            grid.caption(column, row, text.c_str(), 20);
        }
    }
    // Arcs joins between curves: each side is extended along its own curvature.
    for (uint32_t column = 0; column < 5; ++column) {
        PrRect c = grid.cell(column, 3, 20);
        float bend = 10 + float(column) * 12;
        PrPath* path = prPathCreate();
        prPathMoveTo(path, PrVec2{c.left, c.bottom});
        prPathQuadTo(path, PrVec2{c.left + bend, c.top + bend}, PrVec2{(c.left + c.right) / 2, c.top + 10});
        prPathQuadTo(path, PrVec2{c.right - bend, c.top + bend}, PrVec2{c.right, c.bottom});
        PrDrawStroke stroke = strokeOf(16, column % 2 == 0 ? PrPathJoin_Arcs : PrPathJoin_Miter);
        stroke.shape.miterLimit = 6;
        prDrawListPath(list, path, PrPathFillRule_NonZero,
            {.color = withAlpha(column % 2 == 0 ? kGreen : kGold, 0.85f), .stroke = &stroke});
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = kRed, .stroke = &guide});
        prPathDestroy(path);
        grid.caption(column, 3, column % 2 == 0 ? "arcs between curves, limit 6" : "miter between curves, limit 6", 20);
    }
}

void widths(PrDrawList* list, float width, float height, double) {
    float widthsOf[] = {0.1f, 0.25f, 0.5f, 1, 1.5f, 2, 3, 5, 8, 13};
    float top = 30;
    float step = (height - 60) / 10;
    PrVec2 origin{width * 0.72f, height * 0.5f};
    float radius = extentOf(PrRect{0, 0, width * 0.5f, height}) * 0.45f;
    for (uint32_t i = 0; i < 10; ++i) {
        PrDrawStroke stroke = strokeOf(widthsOf[i]);
        float y = top + step * float(i) + step / 2;
        prDrawListLine(list, PrVec2{60, y}, PrVec2{width * 0.42f, y}, {.color = kInk, .stroke = &stroke});
        char text[16];
        std::snprintf(text, sizeof(text), "%g", double(widthsOf[i]));
        caption(PrRect{10, y - 12, 55, y + 5}, text);
        float angle = kPi * float(i) / 10;
        PrVec2 end{origin.x + radius * std::cos(angle), origin.y + radius * std::sin(angle)};
        prDrawListLine(list, origin, end, {.color = kInk, .stroke = &stroke});
    }
    // The same widths under a scale of 3, measured in target pixels where they do not scale.
    prDrawListPushTransformAffine2(list,
        prAffine2Concat(prAffine2Scale(PrVec2{3, 3}), prAffine2Translate(PrVec2{width * 0.46f, 30})));
    for (uint32_t i = 0; i < 4; ++i) {
        PrDrawStroke stroke = strokeOf(2);
        stroke.nonScaling = i % 2 == 1;
        float y = 6 + float(i) * 6;
        prDrawListLine(list, PrVec2{0, y}, PrVec2{20, y}, {.color = i % 2 == 1 ? kRed : kInk, .stroke = &stroke});
    }
    prDrawListPopTransform(list);
    // A stroke under a non-uniform scale is stroked in its own space and stretched.
    prDrawListPushTransformAffine2(list,
        prAffine2Concat(prAffine2Scale(PrVec2{4, 1}), prAffine2Translate(PrVec2{width * 0.46f, 150})));
    PrDrawStroke stretched = strokeOf(6, PrPathJoin_Round);
    prDrawListEllipse(list, PrVec2{10, 40}, PrVec2{8, 30}, 0, {.color = kBlue, .stroke = &stretched});
    prDrawListPopTransform(list);
    float x = width * 0.46f;
    caption(PrRect{x - 60, 110, x + 180, 128}, "width 2 at 3x: ink, and red non-scaling");
    caption(PrRect{x - 60, 234, x + 180, 252}, "stroked under a 4x1 scale");
    caption(PrRect{origin.x - radius, height - 24, origin.x + radius, height - 6}, "widths 0.1 to 13, fanned");
}

void profiles(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    PrDrawStroke guide = hairlineStroke();
    const PrPathWidthPoint taper[] = {{0, 0, 0}, {1, 0.5f, 0.5f}};
    const PrPathWidthPoint bulge[] = {{0, 0.1f, 0.1f}, {0.5f, 1, 1}, {1, 0.1f, 0.1f}};
    const PrPathWidthPoint leftOnly[] = {{0, 1, 0}};
    const PrPathWidthPoint brush[] = {{0, 0, 0}, {0.1f, 0.5f, 0.5f}, {0.7f, 0.35f, 0.35f}, {1, 0, 0}};
    const PrPathWidthPoint wave[] = {
        {0, 0.2f, 0.2f}, {0.2f, 0.6f, 0.1f}, {0.4f, 0.1f, 0.6f},
        {0.6f, 0.6f, 0.1f}, {0.8f, 0.1f, 0.6f}, {1, 0.2f, 0.2f},
    };
    const PrPathWidthPoint* sets[] = {taper, bulge, leftOnly, brush, wave, bulge};
    uint32_t counts[] = {2, 3, 1, 4, 6, 3};
    for (uint32_t i = 0; i < 6; ++i) {
        PrRect c = grid.at(i, 40);
        PrPath* path = prPathCreate();
        if (i == 5) {
            PrVec2 radii{prRectWidth(c) / 2 - 30, prRectHeight(c) / 2 - 30};
            prPathAddEllipse(path, prRectCenter(c), radii, 0);
        } else {
            prPathMoveTo(path, PrVec2{c.left, c.bottom - 20});
            prPathCubicTo(path, PrVec2{c.left + 60, c.top - 40}, PrVec2{c.right - 60, c.bottom + 40},
                PrVec2{c.right, c.top + 20});
        }
        PrDrawStroke stroke = strokeOf(40, PrPathJoin_Round, i == 1 ? PrPathCap_Round : PrPathCap_Butt);
        stroke.shape.profile = sets[i];
        stroke.shape.profileCount = counts[i];
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = withAlpha(kPurple, 0.9f), .stroke = &stroke});
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = kRed, .stroke = &guide});
        prPathDestroy(path);
        const char* names[] = {
            "taper",      "bulge, round caps", "left side only",
            "brush stroke", "alternating sides", "bulge around a closed ellipse",
        };
        grid.captionAt(i, names[i], 40);
    }
}

// Returns a wavy open curve across the cell.
PrPath* wave(PrRect c) {
    PrPath* path = prPathCreate();
    float w = prRectWidth(c);
    float y = (c.top + c.bottom) / 2;
    prPathMoveTo(path, PrVec2{c.left, y});
    prPathCubicTo(path, PrVec2{c.left + w * 0.25f, y - 90}, PrVec2{c.left + w * 0.25f, y + 90}, PrVec2{c.left + w * 0.5f, y});
    prPathCubicTo(path, PrVec2{c.left + w * 0.75f, y - 90}, PrVec2{c.left + w * 0.75f, y + 90}, PrVec2{c.right, y});
    return path;
}

void strokeMapping(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 2, 3};
    const PrDrawColorStop rainbow[] = {
        {.offset = 0, .color = PrColor{0.9f, 0.1f, 0.1f, 1}},
        {.offset = 0.25f, .color = PrColor{0.95f, 0.8f, 0.1f, 1}},
        {.offset = 0.5f, .color = PrColor{0.1f, 0.7f, 0.3f, 1}},
        {.offset = 0.75f, .color = PrColor{0.1f, 0.4f, 0.9f, 1}},
        {.offset = 1, .color = PrColor{0.6f, 0.2f, 0.8f, 1}},
    };
    const PrDrawColorStop tube[] = {
        {.offset = 0, .color = PrColor{0.05f, 0.1f, 0.3f, 1}},
        {.offset = 0.35f, .color = PrColor{0.5f, 0.7f, 1, 1}},
        {.offset = 1, .color = PrColor{0.05f, 0.1f, 0.3f, 1}},
    };
    const PrDrawColorStop bands[] = {
        {.offset = 0, .color = kGold},
        {.offset = 0.5f, .color = kGold},
        {.offset = 0.5f, .color = kInk},
        {.offset = 1, .color = kInk},
    };
    struct Case {
        const PrDrawColorStop* stops;
        uint32_t count;
        PrDrawStrokeMapping mapping;
        PrDrawExtend extend;
        float repeats;
    };
    const Case cases[] = {
        {rainbow, 5, PrDrawStrokeMapping_Plain, PrDrawExtend_Pad, 1},
        {rainbow, 5, PrDrawStrokeMapping_Along, PrDrawExtend_Pad, 1},
        {tube, 3, PrDrawStrokeMapping_Across, PrDrawExtend_Pad, 1},
        {bands, 4, PrDrawStrokeMapping_Along, PrDrawExtend_Repeat, 16},
    };
    for (uint32_t i = 0; i < 4; ++i) {
        PrRect c = grid.at(i, 40);
        bool plain = cases[i].mapping == PrDrawStrokeMapping_Plain;
        PrDrawBrush brush;
        if (plain)
            brush = prDrawBrushLinear(PrVec2{c.left, 0}, PrVec2{c.right, 0}, cases[i].stops, cases[i].count);
        else
            brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{1 / cases[i].repeats, 0}, cases[i].stops, cases[i].count);
        brush.interpolationSpace = PrColorSpace_Oklab;
        brush.extend = cases[i].extend;
        PrDrawStroke stroke = strokeOf(28, PrPathJoin_Round, PrPathCap_Round);
        stroke.mapping = cases[i].mapping;
        PrPath* path = wave(c);
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.brush = &brush, .stroke = &stroke});
        prPathDestroy(path);
        const char* names[] = {
            "plain: the brush in its own space", "along the stroke",
            "across the stroke", "bands repeating along",
        };
        grid.captionAt(i, names[i], 40);
    }
    // A tapered stroke shaded across, and a circle with a rainbow along it.
    PrRect c = grid.at(4, 40);
    PrDrawBrush across = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{1, 0}, tube, 3);
    const PrPathWidthPoint taper[] = {{0, 0.05f, 0.05f}, {0.5f, 0.5f, 0.5f}, {1, 0.05f, 0.05f}};
    PrDrawStroke tapered = strokeOf(50, PrPathJoin_Round, PrPathCap_Round);
    tapered.shape.profile = taper;
    tapered.shape.profileCount = 3;
    tapered.mapping = PrDrawStrokeMapping_Across;
    PrPath* path = wave(c);
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.brush = &across, .stroke = &tapered});
    prPathDestroy(path);
    c = grid.at(5, 40);
    const PrDrawColorStop loop[] = {{.offset = 0, .color = kRed}, {.offset = 1, .color = kRed}};
    PrDrawBrush along = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{1, 0}, loop, 2);
    along.interpolationSpace = PrColorSpace_Oklch;
    along.hueMethod = PrColorHueMethod_Longer;
    PrDrawStroke ring = strokeOf(22);
    ring.mapping = PrDrawStrokeMapping_Along;
    prDrawListEllipse(list, prRectCenter(c),
        PrVec2{prRectWidth(c) / 2 - 20, prRectHeight(c) / 2}, 0, {.brush = &across, .stroke = &ring});
    grid.captionAt(4, "tapered, shaded across", 40);
    grid.captionAt(5, "hue turning once along a ring", 40);
}

void dashes(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    float patterns[][4] = {
        {20, 10, 0, 0}, {30, 10, 5, 10}, {1, 12, 0, 0},
        {0, 16, 0, 0},  {40, 20, 0, 0},  {15, 0, 0, 0},
    };
    uint32_t counts[] = {2, 4, 2, 2, 2, 1};
    PrPathCap caps[] = {
        PrPathCap_Butt,  PrPathCap_Butt,   PrPathCap_Round,
        PrPathCap_Round, PrPathCap_Square, PrPathCap_Triangle,
    };
    for (uint32_t i = 0; i < 6; ++i) {
        PrRect c = grid.cell(i % 3, i / 3, 40);
        PrDrawStroke stroke = strokeOf(8, PrPathJoin_Round, caps[i]);
        stroke.shape.dashes = patterns[i];
        stroke.shape.dashCount = counts[i];
        stroke.shape.dashOffset = i == 4 ? 20 : 0;
        float r = extentOf(c) / 2;
        PrVec2 center = prRectCenter(c);
        PrRect square{center.x - r, center.y - r, center.x + r, center.y + r};
        prDrawListCornerRect(list, prCornerRectRound(square, r / 3), {.color = kInk, .stroke = &stroke});
        prDrawListEllipse(list, center, PrVec2{r / 2, r / 2}, 0, {.color = kRed, .stroke = &stroke});
        const char* names[] = {
            "20 10, butt",         "30 10 5 10, butt", "1 12, round: dots",
            "0 16, round: zero-length dots", "40 20, square, offset 20",
            "15 (odd, doubled), triangle",
        };
        grid.captionAt(i, names[i], 40);
    }
}

void alignment(PrDrawList* list, float width, float height, double) {
    // Columns are inside, center and outside. Rows are a rounded rect, a star and an open curve.
    Grid grid{width, height, 3, 3};
    PrDrawStrokeAlign aligns[] = {PrDrawStrokeAlign_Inside, PrDrawStrokeAlign_Center, PrDrawStrokeAlign_Outside};
    PrDrawStroke guide = hairlineStroke();
    for (uint32_t column = 0; column < 3; ++column) {
        PrDrawStroke stroke = strokeOf(14, PrPathJoin_Miter);
        stroke.align = aligns[column];
        PrRect c = grid.cell(column, 0, 40);
        PrCornerRect rrect = prCornerRectRound(c, 30);
        prDrawListCornerRect(list, rrect, {.color = withAlpha(kGold, 0.4f)});
        prDrawListCornerRect(list, rrect, {.color = withAlpha(kBlue, 0.8f), .stroke = &stroke});
        prDrawListCornerRect(list, rrect, {.color = kRed, .stroke = &guide});

        c = grid.cell(column, 1, 30);
        PrPath* path = star(prRectCenter(c), extentOf(c) / 2, 5, 2);
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = withAlpha(kGreen, 0.8f), .stroke = &stroke});
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = kRed, .stroke = &guide});
        prPathDestroy(path);

        c = grid.cell(column, 2, 40);
        path = prPathCreate();
        prPathMoveTo(path, PrVec2{c.left, c.bottom});
        prPathQuadTo(path, prRectCenter(PrRect{c.left, c.top - 80, c.right, c.bottom}), PrVec2{c.right, c.bottom});
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = withAlpha(kPurple, 0.8f), .stroke = &stroke});
        prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = kRed, .stroke = &guide});
        prPathDestroy(path);
        const char* names[] = {"inside", "center", "outside"};
        grid.caption(column, 0, names[column], 40);
        grid.caption(column, 1, names[column], 30);
        grid.caption(column, 2, "open: always centered", 40);
    }
}

void transforms(PrDrawList* list, float width, float height, double) {
    PrVec2 center{width / 2, height / 2};
    PrDrawStroke outline = strokeOf(2);
    for (uint32_t i = 0; i < 12; ++i) {
        float angle = 2 * kPi * float(i) / 12;
        prDrawListPushTransformAffine2(list, prAffine2Translate(center));
        prDrawListPushTransformAffine2(list, prAffine2Rotate(angle));
        prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{extentOf(PrRect{0, 0, width, height}) * 0.3f, 0}));
        prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{1 + float(i) * 0.08f, 0.6f}));
        prDrawListRect(list, PrRect{-20, -20, 20, 20}, {.color = withAlpha(i % 2 == 0 ? kBlue : kGreen, 0.8f)});
        prDrawListRect(list, PrRect{-20, -20, 20, 20}, {.color = kInk, .stroke = &outline});
        prDrawListPopTransform(list);
        prDrawListPopTransform(list);
        prDrawListPopTransform(list);
        prDrawListPopTransform(list);
    }
    PrAffine2 skew{1, 0, 0.5f, 1, 0, 0};
    prDrawListPushTransformAffine2(list, prAffine2Concat(skew, prAffine2Translate(PrVec2{center.x - 60, center.y - 40})));
    prDrawListRect(list, PrRect{0, 0, 80, 80}, {.color = kGold});
    prDrawListRect(list, PrRect{0, 0, 80, 80}, {.color = kInk, .stroke = &outline});
    prDrawListPopTransform(list);
    // A singular transform draws nothing until its pop.
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{0, 1}));
    prDrawListRect(list, PrRect{0, 0, width, height}, {.color = kRed});
    prDrawListPopTransform(list);
    caption(PrRect{center.x - 150, center.y + 50, center.x + 150, center.y + 68},
        "skewed square; a singular transform hides a red page");
    caption(PrRect{0, height - 24, width, height - 6}, "ring: translate, rotate, translate, scale, nested");
}

// Returns a projection that places the plane's origin at `at`, with a perspective distance of `distance`, after `turn`.
PrMatrix4 staged(PrVec2 at, float distance, const PrMatrix4& turn) {
    PrMatrix4 perspective = prMatrix4Perspective(distance);
    PrMatrix4 place = prMatrix4Translate(PrVec3{at.x, at.y, 0});
    PrMatrix4 m = prMatrix4Concat(&turn, &perspective);
    return prMatrix4Concat(&m, &place);
}

// Pushes a projected group whose plane is drawn through `projection` within `bounds`, with `opacity`.
void pushPlane(PrDrawList* list, const PrMatrix4& projection, PrRect bounds, float opacity = 1,
        PrDrawBlendMode blendMode = PrDrawBlendMode_Normal) {
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = bounds;
    group.projection = &projection;
    group.opacity = opacity;
    group.blendMode = blendMode;
    prDrawListPushGroup(list, &group);
}

// Returns the rectangle of `w` by `h` centered on the origin, grown by `margin` on every side.
PrRect centered(float w, float h, float margin = 0) {
    return PrRect{-w / 2 - margin, -h / 2 - margin, w / 2 + margin, h / 2 + margin};
}

// The width of the outline that `card` strokes.
constexpr float kCardOutline = 4;

void card(PrDrawList* list, float w, float h) {
    PrRect r = centered(w, h);
    const PrDrawColorStop stops[] = {
        {.offset = 0, .color = PrColor{0.95f, 0.75f, 0.2f, 1}},
        {.offset = 1, .color = PrColor{0.25f, 0.2f, 0.6f, 1}},
    };
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{r.left, r.top}, PrVec2{r.right, r.bottom}, stops, 2);
    prDrawListCornerRect(list, prCornerRectRound(r, 16), {.brush = &brush});
    drawImage(list, testCard(), PrRect{0, 0, 64, 64}, prRectAt(PrVec2{r.left + 16, r.top + 16}, PrVec2{h / 2, h / 2}),
        PrDrawSampling_Linear, 1);
    PrDrawStroke outline = strokeOf(kCardOutline, PrPathJoin_Round);
    prDrawListCornerRect(list, prCornerRectRound(r, 16), {.color = kInk, .stroke = &outline});
    PrPath* path = star(PrVec2{r.right - h / 4, 0}, h / 5, 5, 2);
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = kRed});
    prPathDestroy(path);
}

// Draws `card` of `w` by `h` as a projected group through `projection`.
void cardPlane(PrDrawList* list, const PrMatrix4& projection, float w, float h, float opacity = 1,
        PrDrawBlendMode blendMode = PrDrawBlendMode_Normal) {
    pushPlane(list, projection, centered(w, h, kCardOutline / 2), opacity, blendMode);
    card(list, w, h);
    prDrawListPopGroup(list);
}

const PrDrawFilter* blurOf(PrVec2 sigma) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = sigma;
    prDrawFilterBuilderAdd(builder, &blur);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

void transforms3d(PrDrawList* list, float width, float height, double) {
    // A card turned about the vertical axis in perspective.
    PrMatrix4 turn = prMatrix4Rotate(PrVec3{0, 1, 0}, 0.9f);
    PrMatrix4 m = staged(PrVec2{width * 0.2f, height * 0.25f}, 500, turn);
    cardPlane(list, m, 240, 160);

    // A cube: the faces toward the eye, each a square turned into place.
    PrVec2 center{width * 0.5f, height * 0.25f};
    PrMatrix4 spin = prMatrix4Rotate(PrVec3{1, 1, 0}, 0.6f);
    PrColor faces[] = {kRed, kGreen, kBlue, kGold, kPurple, kTeal};
    const float axes[6][4] = {
        {0, 1, 0, 0},     {0, 1, 0, kPi / 2}, {0, 1, 0, kPi},
        {0, 1, 0, -kPi / 2}, {1, 0, 0, kPi / 2}, {1, 0, 0, -kPi / 2},
    };
    PrDrawStroke edge = strokeOf(3);
    for (uint32_t i = 0; i < 6; ++i) {
        PrMatrix4 out = prMatrix4Translate(PrVec3{0, 0, 60});
        PrMatrix4 around = prMatrix4Rotate(PrVec3{axes[i][0], axes[i][1], axes[i][2]}, axes[i][3]);
        PrMatrix4 face = prMatrix4Concat(&out, &around);
        face = prMatrix4Concat(&face, &spin);
        // A face faces the eye if its normal points toward the eye, along +z.
        float nz = face.m[2][2];
        if (nz < 0)
            continue;
        PrMatrix4 whole = staged(center, 600, face);
        pushPlane(list, whole, centered(120, 120, edge.shape.width / 2));
        prDrawListRect(list, centered(120, 120), {.color = withAlpha(faces[i], 0.9f)});
        prDrawListRect(list, centered(120, 120), {.color = kInk, .stroke = &edge});
        prDrawListPopGroup(list);
    }

    // A floor of checkers receding toward the horizon, sampled from mip levels.
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, 1.2f);
    m = staged(PrVec2{width * 0.82f, height * 0.38f}, 300, tilt);
    PrDrawBrush floor = prDrawBrushDefault(PrDrawBrushKind_Image);
    floor.image.source = prDrawSourceImage(smallTile(), PrRect{0, 0, 16, 16});
    floor.image.tiles.tile = PrRect{0, 0, 40, 40};
    floor.image.tiles.x = PrDrawExtend_Repeat;
    floor.image.tiles.y = PrDrawExtend_Repeat;
    floor.image.sampling = PrDrawSampling_Mipmap;
    PrRect floorRect{-300, -700, 300, 120};
    pushPlane(list, m, floorRect);
    prDrawListRect(list, floorRect, {.brush = &floor});
    prDrawListPopGroup(list);

    // A card leaning back past the eye: what lies behind it is cut away.
    turn = prMatrix4Rotate(PrVec3{1, 0, 0}, 1.1f);
    m = staged(PrVec2{width * 0.18f, height * 0.62f}, 150, turn);
    cardPlane(list, m, 240, 400);

    // A clip and a blurred group in perspective: the blur follows the card's plane.
    turn = prMatrix4Rotate(PrVec3{0, 1, 0}, -0.8f);
    m = staged(PrVec2{width * 0.5f, height * 0.72f}, 500, turn);
    pushPlane(list, m, centered(240, 160, kCardOutline / 2));
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, PrVec2{0, 0}, PrVec2{100, 100}, 0);
    prDrawListPushClipPath(list, circle, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prPathDestroy(circle);
    card(list, 240, 160);
    prDrawListPopClip(list);
    prDrawListPopGroup(list);

    turn = prMatrix4Rotate(PrVec3{0, 1, 0}, 0.8f);
    m = staged(PrVec2{width * 0.8f, height * 0.72f}, 500, turn);
    PrRect blurred = centered(320, 240);
    pushPlane(list, m, blurred);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = blurred;
    group.filter = blurOf(PrVec2{6, 0});
    prDrawListPushGroup(list, &group);
    prDrawFilterRelease(group.filter);
    card(list, 240, 160);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    auto under = [&](float x, float y, const char* text) {
        caption(PrRect{x - 170, y, x + 170, y + 18}, text);
    };
    under(width * 0.2f, height * 0.25f + 110, "turned about y, in perspective");
    under(width * 0.5f, height * 0.25f + 110, "a cube's faces toward the eye");
    under(width * 0.82f, height * 0.5f, "image tiles receding, from mip levels");
    under(width * 0.18f, height - 24, "leaning back past the eye: cut away");
    under(width * 0.5f, height - 24, "clipped to a circle in its plane");
    under(width * 0.8f, height - 24, "blurred along its own x axis");
}

// Fills `cell` with diagonal stripes, for what a projected group lands over.
void stripes(PrDrawList* list, PrRect cell) {
    prDrawListPushClipRect(list, cell, PrDrawEdges_Antialiased);
    float extent = prRectWidth(cell) + prRectHeight(cell);
    PrPath* bands = prPathCreate();
    for (float x = 0; x < extent; x += 24) {
        prPathMoveTo(bands, PrVec2{cell.left + x, cell.top});
        prPathLineTo(bands, PrVec2{cell.left + x + 12, cell.top});
        prPathLineTo(bands, PrVec2{cell.left + x + 12 - prRectHeight(cell), cell.bottom});
        prPathLineTo(bands, PrVec2{cell.left + x - prRectHeight(cell), cell.bottom});
        prPathClose(bands);
    }
    prDrawListPath(list, bands, PrPathFillRule_NonZero, {.color = withAlpha(kTeal, 0.6f)});
    prPathDestroy(bands);
    prDrawListPopClip(list);
}

void projectedGroups(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    PrMatrix4 turn = prMatrix4Rotate(PrVec3{0, 1, 0}, 0.7f);
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, 0.6f);
    auto stagedIn = [&](PrRect cell, const PrMatrix4& by) {
        return staged(prRectCenter(cell), 500, by);
    };

    // Opacity and a blend mode apply where the plane lands, over stripes drawn before it.
    PrRect cell = grid.at(0);
    stripes(list, cell);
    cardPlane(list, stagedIn(cell, turn), 240, 160, 0.5f);
    cell = grid.at(1);
    stripes(list, cell);
    cardPlane(list, stagedIn(cell, turn), 240, 160, 1, PrDrawBlendMode_Multiply);

    // A plane inside a turned plane flattens into it: the inner card turns with the outer frame.
    cell = grid.at(2);
    pushPlane(list, stagedIn(cell, turn), centered(280, 200));
    prDrawListRect(list, centered(280, 200), {.color = withAlpha(kGray, 0.4f)});
    PrMatrix4 inner = staged(PrVec2{0, 0}, 400, tilt);
    cardPlane(list, inner, 200, 130);
    prDrawListPopGroup(list);

    // A clip open at the push cuts the projected plane in target space.
    cell = grid.at(3);
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, prRectCenter(cell), PrVec2{90, 90}, 0);
    prDrawListPushClipPath(list, circle, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prPathDestroy(circle);
    cardPlane(list, stagedIn(cell, turn), 240, 160);
    prDrawListPopClip(list);

    // A backdrop inside the plane reads only what the plane drew before it, not the stripes beneath the plane.
    cell = grid.at(4);
    stripes(list, cell);
    pushPlane(list, stagedIn(cell, turn), centered(240, 160, kCardOutline / 2));
    card(list, 240, 160);
    PrDrawGroup frosted = prDrawGroupDefault();
    frosted.bounds = PrRect{-120, -20, 120, 82};
    frosted.backdrop = blurOf(PrVec2{8, 8});
    prDrawListPushGroup(list, &frosted);
    prDrawFilterRelease(frosted.backdrop);
    prDrawListRect(list, frosted.bounds, {.color = withAlpha(kWhite, 0.3f)});
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);

    // A plane seen edge-on draws nothing.
    cell = grid.at(5);
    PrMatrix4 edgeOn = stagedIn(cell, prMatrix4Rotate(PrVec3{0, 1, 0}, kPi / 2));
    pushPlane(list, edgeOn, centered(240, 160));
    prDrawListRect(list, centered(240, 160), {.color = kRed});
    prDrawListPopGroup(list);

    const char* captions[] = {
        "half opacity, where the plane lands",
        "multiply, where the plane lands",
        "a plane inside a turned plane",
        "a circle clip open at the push",
        "a backdrop in the plane reads only the plane",
        "edge-on: a red plane draws nothing",
    };
    for (uint32_t i = 0; i < 6; ++i)
        grid.captionAt(i, captions[i]);
}

void pathOperations(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    const PrPathPolylineOp ops[] = {
        PrPathPolylineOp_Union, PrPathPolylineOp_Intersect, PrPathPolylineOp_Difference,
        PrPathPolylineOp_Xor,
    };
    const char* names[] = {"union", "intersect", "difference", "exclusion"};
    PrDrawStroke hairline = strokeOf(1);
    for (uint32_t i = 0; i < 4; ++i) {
        PrRect c = grid.at(i, 30);
        PrVec2 center = prRectCenter(c);
        float r = extentOf(c) * 0.3f;
        PrPath* a = prPathCreate();
        prPathAddEllipse(a, PrVec2{center.x - r * 0.3f, center.y}, PrVec2{r, r}, 0);
        PrPath* b = prPathCreate();
        prPathAddRect(b, PrRect{center.x - r * 0.2f, center.y - r * 0.8f, center.x + r * 1.3f, center.y + r * 1.1f});
        PrPathPolyline flatA{};
        PrPathPolyline flatB{};
        PrPathPolyline out{};
        prPathFlatten(a, prAffine2Identity(), 0.1f, PrPathFlattening_Inside, &flatA);
        prPathFlatten(b, prAffine2Identity(), 0.1f, PrPathFlattening_Inside, &flatB);
        prPathPolylineCombine(&flatA, PrPathFillRule_NonZero, &flatB, PrPathFillRule_NonZero, ops[i], &out);
        prDrawListPolyline(list, &out, PrPathFillRule_NonZero, {.color = withAlpha(kTeal, 0.8f)});
        prDrawListPath(list, a, PrPathFillRule_NonZero, {.color = withAlpha(kInk, 0.4f), .stroke = &hairline});
        prDrawListPath(list, b, PrPathFillRule_NonZero, {.color = withAlpha(kInk, 0.4f), .stroke = &hairline});
        grid.captionAt(i, names[i], 30);
        prPathPolylineReset(&out);
        prPathPolylineReset(&flatB);
        prPathPolylineReset(&flatA);
        prPathDestroy(b);
        prPathDestroy(a);
    }
    // A star's overlaps resolved by each rule, then offset outward and inward by each join.
    struct Offset {
        float distance;
        PrPathJoin join;
        const char* name;
    };
    const Offset offsets[] = {
        {0, PrPathJoin_Miter, "the star, even-odd, resolved"},
        {10, PrPathJoin_Miter, "grown 10, mitered"},
        {10, PrPathJoin_Round, "grown 10, round"},
        {-6, PrPathJoin_Round, "the resolved star shrunk 6, round"},
    };
    for (uint32_t i = 0; i < 4; ++i) {
        PrRect c = grid.at(4 + i, 30);
        PrPath* shape = star(prRectCenter(c), extentOf(c) * 0.32f, 5, 2);
        PrPathPolyline flat{};
        PrPathPolyline resolved{};
        PrPathPolyline empty{};
        PrPathPolyline out{};
        prPathFlatten(shape, prAffine2Identity(), 0.1f, PrPathFlattening_Inside, &flat);
        prPathPolylineCombine(&flat, PrPathFillRule_EvenOdd, &empty, PrPathFillRule_NonZero, PrPathPolylineOp_Union,
            &resolved);
        prPathPolylineOffset(i == 0 ? &flat : &resolved, i == 0 ? PrPathFillRule_EvenOdd : PrPathFillRule_NonZero,
            offsets[i].distance, offsets[i].join, 4, 0.1f, &out);
        prDrawListPolyline(list, &out, PrPathFillRule_NonZero, {.color = withAlpha(kPurple, 0.75f)});
        prDrawListPath(list, shape, PrPathFillRule_NonZero, {.color = withAlpha(kInk, 0.5f), .stroke = &hairline});
        grid.captionAt(4 + i, offsets[i].name, 30);
        prPathPolylineReset(&out);
        prPathPolylineReset(&resolved);
        prPathPolylineReset(&flat);
        prPathDestroy(shape);
    }
}

void antialiasing(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 2, 1};
    for (uint32_t column = 0; column < 2; ++column) {
        PrRect c = grid.cell(column, 0, 30);
        PrVec2 center = prRectCenter(c);
        PrDrawEdges edges = column == 0 ? PrDrawEdges_Antialiased : PrDrawEdges_Aliased;
        PrDrawStroke stroke = strokeOf(1);
        for (uint32_t i = 0; i < 24; ++i) {
            float angle = kPi * float(i) / 24;
            float r = extentOf(c) / 2;
            prDrawListLine(list, center, PrVec2{center.x + r * std::cos(angle), center.y - r * std::sin(angle)},
                {.color = kInk, .stroke = &stroke, .edges = edges});
        }
        prDrawListEllipse(list, PrVec2{center.x, center.y + extentOf(c) / 4}, PrVec2{extentOf(c) / 5, extentOf(c) / 9},
            0.2f, {.color = kRed, .edges = edges});
        grid.caption(column, 0, column == 0 ? "antialiased" : "aliased", 30);
    }
}

} // namespace

const std::vector<Drawing>& shapeFeatures() {
    static const std::vector<Drawing> features = {
        {"Fills", "Every shape kind filled, and translucent fills overlapping.", fills},
        {"Fill rules", "Self-intersecting and nested contours under each fill rule.", fillRules},
        {
            "Corners",
            "Superellipse corners across the range of K, then mixed and fitted corners, filled, stroked and dashed.",
            corners,
        },
        {"Sectors and capsules", "Sectors and rings, then capsules, filled and stroked.", arcsAndCapsules},
        {
            "Joins and caps",
            "Every join by every cap at miter limit 2; the red hairline is the centerline.",
            joinsAndCaps,
        },
        {"Miter limits", "Sharper turns at limit 3 under each miter join, and joins between curves.", miterLimits},
        {"Widths", "Stroke widths from the hairline up, and widths under scaling transforms.", widths},
        {
            "Width profiles",
            "Half widths varying along each contour, on either side; the red hairline is the centerline.",
            profiles,
        },
        {"Stroke brush mapping", "Brushes placed on strokes in their own space, along and across.", strokeMapping},
        {"Dashes", "Dash patterns with each cap; a closed contour joins its last dash to its first.", dashes},
        {
            "Stroke alignment",
            "Width 14 inside, centered and outside a closed contour; the red hairline is the shape.",
            alignment,
        },
        {"Transforms", "Nested transform pushes, a skew, and a transform that collapses the plane.", transforms},
        {
            "Transforms in 3D",
            "Shapes, images, clips and a filtered group on planes turned in perspective.",
            transforms3d,
        },
        {
            "Projected groups",
            "Planes in perspective under opacity, a blend mode, a clip, a nested plane and a backdrop.",
            projectedGroups,
        },
        {
            "Path operations",
            "Regions united, intersected, subtracted and excluded; resolved, grown and shrunk.",
            pathOperations,
        },
        {"Antialiasing", "The same lines and ellipse with and without antialiasing.", antialiasing},
    };
    return features;
}

} // namespace viewer
