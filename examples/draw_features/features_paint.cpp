// Feature drawings for paint: gradients, images and image brushes.

#include "examples/draw_features/features_paint.h"

#include "pr/color_space.h"
#include "pr/compress_builtin.h"
#include "pr/count_of.h"
#include "pr/draw_filter.h"
#include "pr/draw_renderer.h"
#include "pr/image.h"
#include "pr/image_builtin.h"
#include "pr/string.h"
#include "pr/vec.h"

#include "examples/draw_features/drawing.h"

using namespace pr;

namespace drawings {
namespace {

constexpr PrDrawColorStop kRainbow[] = {
    {.offset = 0, .color = PrColor{0.9f, 0.1f, 0.1f, 1}},
    {.offset = 0.25f, .color = PrColor{0.95f, 0.8f, 0.1f, 1}},
    {.offset = 0.5f, .color = PrColor{0.1f, 0.7f, 0.3f, 1}},
    {.offset = 0.75f, .color = PrColor{0.1f, 0.4f, 0.9f, 1}},
    {.offset = 1, .color = PrColor{0.6f, 0.2f, 0.8f, 1}},
};

constexpr PrDrawColorStop kTwo[] = {
    {.offset = 0, .color = PrColor{0.05f, 0.2f, 0.6f, 1}},
    {.offset = 1, .color = PrColor{1, 0.85f, 0.3f, 1}},
};

void linearGradients(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 3};
    PrDrawExtend extends[] = {PrDrawExtend_Pad, PrDrawExtend_Reflect, PrDrawExtend_Repeat, PrDrawExtend_None};
    for (uint32_t i = 0; i < 4; ++i) {
        PrRect c = grid.at(i);
        float third = prRectWidth(c) / 3;
        PrDrawBrush brush = prDrawBrushLinear(PrVec2{c.left + third, c.top}, PrVec2{c.left + 2 * third, c.top}, kTwo, 2);
        brush.extend = extends[i];
        prDrawListRect(list, c, {.brush = &brush});
    }
    // Hard stops, hints, many stops.
    PrRect c = grid.at(4);
    const PrDrawColorStop hard[] = {
        {.offset = 0, .color = kRed},
        {.offset = 0.33f, .color = kRed},
        {.offset = 0.33f, .color = kGold},
        {.offset = 0.66f, .color = kGold},
        {.offset = 0.66f, .color = kBlue},
        {.offset = 1, .color = kBlue},
    };
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{c.left, c.top}, PrVec2{c.right, c.top}, hard, 6);
    prDrawListRect(list, c, {.brush = &brush});
    c = grid.at(5);
    for (uint32_t k = 0; k < 4; ++k) {
        float band = prRectHeight(c) / 4;
        PrDrawColorStop hinted[] = {
            {.offset = 0, .color = kInk, .hint = 0.1f + 0.25f * float(k)},
            {.offset = 1, .color = kWhite},
        };
        PrRect r{c.left, c.top + band * float(k), c.right, c.top + band * float(k + 1)};
        brush = prDrawBrushLinear(PrVec2{c.left, 0}, PrVec2{c.right, 0}, hinted, 2);
        prDrawListRect(list, r, {.brush = &brush});
    }
    c = grid.at(6);
    brush = prDrawBrushLinear(PrVec2{c.left, c.bottom}, PrVec2{c.right, c.top}, kRainbow, 5);
    prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) / 2, extentOf(c) / 2}, 0, {.brush = &brush});
    // Transparency, opacity, a stroke.
    c = grid.at(7);
    const PrDrawColorStop fade[] = {{.offset = 0, .color = withAlpha(kBlue, 0)}, {.offset = 1, .color = kBlue}};
    for (float x = c.left; x < c.right; x += 20)
        prDrawListRect(list, PrRect{x, c.top, x + 10, c.bottom}, {.color = kGold});
    brush = prDrawBrushLinear(PrVec2{c.left, 0}, PrVec2{c.right, 0}, fade, 2);
    prDrawListRect(list, c, {.brush = &brush});
    c = grid.at(8);
    for (float x = c.left; x < c.right; x += 20)
        prDrawListRect(list, PrRect{x, c.top, x + 10, c.bottom}, {.color = kGold});
    brush = prDrawBrushLinear(PrVec2{c.left, 0}, PrVec2{c.right, 0}, kRainbow, 5);
    brush.opacity = 0.5f;
    prDrawListRect(list, PrRect{c.left, c.top, c.right, (c.top + c.bottom) / 2}, {.brush = &brush});
    brush.opacity = 0.25f;
    prDrawListRect(list, PrRect{c.left, (c.top + c.bottom) / 2, c.right, c.bottom}, {.brush = &brush});
    c = grid.at(9);
    PrDrawStroke stroke = strokeOf(20, PrPathJoin_Round, PrPathCap_Round);
    PrPath* path = star(prRectCenter(c), extentOf(c) / 2 - 10, 5, 2);
    brush = prDrawBrushLinear(PrVec2{c.left, c.top}, PrVec2{c.right, c.bottom}, kRainbow, 5);
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.brush = &brush, .stroke = &stroke});
    prPathDestroy(path);
    const char* captions[] = {
        "pad over the middle third",  "reflect",
        "repeat",                     "none past the middle third",
        "hard stops",                 "hints 0.1, 0.35, 0.6, 0.85",
        "five stops",                 "fading to transparent over stripes",
        "at opacity 0.5, and 0.25", "stroked",
    };
    for (uint32_t i = 0; i < 10; ++i)
        grid.captionAt(i, captions[i]);
}

void radialGradients(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    PrDrawExtend extends[] = {PrDrawExtend_Pad, PrDrawExtend_Reflect, PrDrawExtend_Repeat};
    for (uint32_t i = 0; i < 3; ++i) {
        PrRect c = grid.at(i);
        PrDrawBrush brush = prDrawBrushRadial(prRectCenter(c), 0, prRectCenter(c), extentOf(c) / 4, kRainbow, 5);
        brush.extend = extends[i];
        prDrawListRect(list, c, {.brush = &brush});
    }
    // An off-center focal point, a cone of two circles, and an ellipse by the brush transform.
    PrRect c = grid.at(3);
    PrVec2 focal{prRectCenter(c).x - 50, prRectCenter(c).y - 50};
    PrDrawBrush brush = prDrawBrushRadial(focal, 0, prRectCenter(c), extentOf(c) / 2, kTwo, 2);
    prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) / 2, extentOf(c) / 2}, 0, {.brush = &brush});
    c = grid.at(4);
    brush = prDrawBrushRadial(PrVec2{c.left + 40, prRectCenter(c).y}, 15, PrVec2{c.right - 60, prRectCenter(c).y}, 50,
        kRainbow, 5);
    prDrawListRect(list, c, {.brush = &brush});
    c = grid.at(5);
    brush = prDrawBrushRadial(PrVec2{0.5f, 0.5f}, 0, PrVec2{0.5f, 0.5f}, 0.5f, kRainbow, 5);
    brush.space = PrDrawBrushSpace_ObjectBounds;
    brush.extend = PrDrawExtend_Reflect;
    prDrawListRect(list, PrRect{c.left, c.top + 40, c.right, c.bottom - 40}, {.brush = &brush});
    const char* captions[] = {
        "pad", "reflect", "repeat", "focal point up and left",
        "a cone between two circles", "in object bounds, reflecting",
    };
    for (uint32_t i = 0; i < 6; ++i)
        grid.captionAt(i, captions[i]);
}

void conicAndDiamond(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    PrRect c = grid.at(0);
    PrDrawColorStop wheel[7];
    for (uint32_t i = 0; i < 7; ++i) {
        float hue = float(i % 6) / 6;
        float r = prF32Abs(hue * 6 - 3) - 1;
        float g = 2 - prF32Abs(hue * 6 - 2);
        float b = 2 - prF32Abs(hue * 6 - 4);
        auto clamp = [](float v) { return v < 0 ? 0 : v > 1 ? 1 : v; };
        wheel[i] = PrDrawColorStop{.offset = float(i) / 6, .color = PrColor{clamp(r), clamp(g), clamp(b), 1}};
    }
    PrDrawBrush brush = prDrawBrushConic(prRectCenter(c), 0, wheel, 7);
    prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) / 2, extentOf(c) / 2}, 0, {.brush = &brush});
    c = grid.at(1);
    const PrDrawColorStop pie[] = {
        {.offset = 0, .color = kRed},
        {.offset = 0.25f, .color = kRed},
        {.offset = 0.25f, .color = kGold},
        {.offset = 0.6f, .color = kGold},
        {.offset = 0.6f, .color = kBlue},
        {.offset = 1, .color = kBlue},
    };
    brush = prDrawBrushConic(prRectCenter(c), -kPi / 2, pie, 6);
    prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) / 2, extentOf(c) / 2}, 0, {.brush = &brush});
    c = grid.at(2);
    const PrDrawColorStop spokes[] = {
        {.offset = 0, .color = kInk},
        {.offset = 0.5f, .color = kWhite},
        {.offset = 1, .color = kInk},
    };
    brush = prDrawBrushConic(PrVec2{c.left + 60, c.top + 60}, kPi / 4, spokes, 3);
    prDrawListRect(list, c, {.brush = &brush});
    PrDrawExtend extends[] = {PrDrawExtend_Pad, PrDrawExtend_Reflect, PrDrawExtend_Repeat};
    for (uint32_t i = 0; i < 3; ++i) {
        c = grid.at(3 + i);
        brush = prDrawBrushDiamond(prRectCenter(c), PrVec2{80, 50}, kRainbow, 5);
        brush.extend = extends[i];
        prDrawListRect(list, c, {.brush = &brush});
    }
    const char* captions[] = {
        "conic color wheel", "conic hard stops from the top",
        "conic centered off the shape", "diamond, pad",
        "diamond, reflect", "diamond, repeat",
    };
    for (uint32_t i = 0; i < 6; ++i)
        grid.captionAt(i, captions[i]);
}

void interpolation(PrDrawList* list, float width, float height, double) {
    // Rows of one gradient in each space, from a saturated blue to a saturated yellow, then red to cyan in each hue
    // direction.
    struct Row {
        PrColorSpace space;
        PrColorHueMethod hueMethod;
    };
    const Row rows[] = {
        {PrColorSpace_Srgb, PrColorHueMethod_Shorter},       {PrColorSpace_SrgbLinear, PrColorHueMethod_Shorter},
        {PrColorSpace_DisplayP3, PrColorHueMethod_Shorter},  {PrColorSpace_Lab, PrColorHueMethod_Shorter},
        {PrColorSpace_Oklab, PrColorHueMethod_Shorter},      {PrColorSpace_XyzD65, PrColorHueMethod_Shorter},
        {PrColorSpace_Lch, PrColorHueMethod_Shorter},        {PrColorSpace_Oklch, PrColorHueMethod_Shorter},
        {PrColorSpace_Hsl, PrColorHueMethod_Shorter},        {PrColorSpace_Hwb, PrColorHueMethod_Shorter},
        {PrColorSpace_Oklch, PrColorHueMethod_Longer},       {PrColorSpace_Oklch, PrColorHueMethod_Increasing},
        {PrColorSpace_Oklch, PrColorHueMethod_Decreasing},
    };
    uint32_t count = uint32_t(countOf(rows));
    float step = (height - 40) / float(count);
    for (uint32_t i = 0; i < count; ++i) {
        bool hues = i >= 10;
        PrDrawColorStop stops[] = {
            {.offset = 0, .color = hues ? PrColor{1, 0.1f, 0.1f, 1} : PrColor{0, 0, 1, 1}},
            {.offset = 1, .color = hues ? PrColor{0.1f, 0.9f, 0.9f, 1} : PrColor{1, 1, 0, 1}},
        };
        PrRect r{170, 20 + step * float(i) + 3, width - 40, 20 + step * float(i + 1) - 3};
        const char* names[] = {
            "sRGB",       "sRGB-linear", "Display P3",  "Lab",
            "Oklab",      "XYZ D65",     "LCH",         "Oklch",
            "HSL",        "HWB",         "Oklch longer", "Oklch increasing",
            "Oklch decreasing",
        };
        caption(PrRect{10, r.top, 160, (r.top + r.bottom) / 2 + 9}, names[i]);
        PrDrawBrush brush = prDrawBrushLinear(PrVec2{r.left, 0}, PrVec2{r.right, 0}, stops, 2);
        brush.interpolationSpace = rows[i].space;
        brush.hueMethod = rows[i].hueMethod;
        prDrawListRect(list, r, {.brush = &brush});
    }
}

void brushSpaces(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    // One gradient in user space spanning three shapes, then in each shape's bounds, then in target pixels under a
    // rotation.
    PrDrawBrush user = prDrawBrushLinear(PrVec2{40, 0}, PrVec2{width - 40, 0}, kRainbow, 5);
    for (uint32_t i = 0; i < 3; ++i) {
        PrRect c = grid.at(i, 40);
        prDrawListCornerRect(list, prCornerRectRound(c, 20), {.brush = &user});
    }
    PrDrawBrush bounds = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{1, 0}, kRainbow, 5);
    bounds.space = PrDrawBrushSpace_ObjectBounds;
    PrRect c = grid.at(3, 40);
    prDrawListRect(list, PrRect{c.left, c.top, c.left + 60, c.bottom}, {.brush = &bounds});
    prDrawListEllipse(list, PrVec2{c.left + 150, prRectCenter(c).y}, PrVec2{70, 90}, 0, {.brush = &bounds});
    // The bounds brush turned a quarter by its transform about the unit square's center.
    c = grid.at(4, 40);
    bounds.transform = prAffine2Concat(prAffine2Concat(prAffine2Translate(PrVec2{-0.5f, -0.5f}), prAffine2Rotate(kPi / 2)),
        prAffine2Translate(PrVec2{0.5f, 0.5f}));
    prDrawListRect(list, c, {.brush = &bounds});
    c = grid.at(5, 40);
    PrDrawBrush inTarget = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{40, 0}, kTwo, 2);
    inTarget.space = PrDrawBrushSpace_Target;
    inTarget.extend = PrDrawExtend_Reflect;
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.4f), prAffine2Translate(prRectCenter(c))));
    prDrawListRect(list, PrRect{-80, -80, 80, 80}, {.brush = &inTarget});
    prDrawListPopTransform(list);
    for (uint32_t i = 0; i < 3; ++i)
        grid.captionAt(i, "one user-space gradient across three shapes", 40);
    grid.captionAt(3, "in each shape's bounds", 40);
    grid.captionAt(4, "in bounds, turned a quarter", 40);
    grid.captionAt(5, "target space: still under a rotation", 40);
}

void images(PrDrawList* list, float width, float height, double) {
    const PrDrawImage* card = testCard();
    Grid grid{width, height, 4, 3};
    PrRect whole{0, 0, 64, 64};
    // Upscaled by each sampling.
    PrDrawSampling samplings[] = {
        PrDrawSampling_Nearest, PrDrawSampling_Linear,
        PrDrawSampling_Cubic, PrDrawSampling_Mipmap,
    };
    for (uint32_t i = 0; i < 4; ++i)
        drawImage(list, card, PrRect{16, 16, 40, 40}, squareIn(grid.at(i, 10), 200), samplings[i], 1);
    // Downscaled: linear aliases where mipmap does not.
    PrRect c = grid.at(4, 10);
    drawImage(list, card, whole, prRectAt(PrVec2{c.left, c.top}, PrVec2{32, 32}), PrDrawSampling_Linear, 1);
    drawImage(list, card, whole, PrRect{c.left, c.top + 40, c.left + 20, c.top + 60}, PrDrawSampling_Linear, 1);
    drawImage(list, card, whole, PrRect{c.left + 60, c.top, c.left + 92, c.top + 32}, PrDrawSampling_Mipmap, 1);
    drawImage(list, card, whole, PrRect{c.left + 60, c.top + 40, c.left + 80, c.top + 60}, PrDrawSampling_Mipmap, 1);
    drawImage(list, card, whole, PrRect{c.left + 120, c.top, c.left + 240, c.top + 120}, PrDrawSampling_Linear, 1);
    // Rotated, tinted, translucent, a source rectangle, stretched.
    c = grid.at(5, 10);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.5f), prAffine2Translate(prRectCenter(c))));
    drawImage(list, card, whole, PrRect{-70, -70, 70, 70}, PrDrawSampling_Linear, 1);
    prDrawListPopTransform(list);
    c = grid.at(6, 10);
    drawImage(list, card, whole, squareIn(c, 180), PrDrawSampling_Linear, 0.5f);
    c = grid.at(7, 10);
    drawImage(list, card, PrRect{0, 0, 32, 32}, squareIn(c, 180), PrDrawSampling_Linear, 1);
    c = grid.at(8, 10);
    drawImage(list, card, whole, inset(c, 20), PrDrawSampling_Linear, 1);
    // Decoded through lib/image, and as the brush of a shape other than its rectangle.
    c = grid.at(9, 10);
    if (const PrDrawImage* decoded = decodedCard())
        drawImage(list, decoded, whole, squareIn(c, 180), PrDrawSampling_Linear, 1);
    c = grid.at(10, 10);
    PrRect rounded = squareIn(c, 180);
    PrDrawSource cardSource = prDrawSourceImage(card, whole);
    PrDrawBrush cardBrush = prDrawBrushImage(cardSource, rounded, PrDrawSampling_Linear);
    prDrawListCornerRect(list, prCornerRectRound(rounded, 40), {.brush = &cardBrush});
    c = grid.at(11, 10);
    PrRect offset{c.left + 0.5f, c.top + 0.5f, c.left + 128.5f, c.top + 128.5f};
    PrDrawBrush nearest = prDrawBrushImage(prDrawSourceImage(card, whole), offset, PrDrawSampling_Nearest);
    prDrawListRect(list, offset, {.brush = &nearest, .edges = PrDrawEdges_Aliased});
    const char* captions[] = {
        "nearest",
        "linear",
        "cubic",
        "mipmap",
        "down: linear, mipmap; up 2x",
        "rotated",
        "tinted, half alpha",
        "a source rectangle",
        "stretched",
        "decoded through lib/image",
        "a rounded rectangle's brush",
        "aliased, nearest, half-pixel offset",
    };
    for (uint32_t i = 0; i < 12; ++i)
        grid.captionAt(i, captions[i], 10);
}

void imageBrushes(PrDrawList* list, float width, float height, double) {
    const PrDrawImage* tile = smallTile();
    Grid grid{width, height, 4, 2};
    PrDrawExtend modes[] = {PrDrawExtend_None, PrDrawExtend_Pad, PrDrawExtend_Repeat, PrDrawExtend_Reflect};
    for (uint32_t i = 0; i < 8; ++i) {
        PrRect c = grid.at(i, 20);
        PrDrawBrush brush = prDrawBrushDefault(PrDrawBrushKind_Image);
        brush.image.source = prDrawSourceImage(tile, PrRect{0, 0, 16, 16});
        PrVec2 center = prRectCenter(c);
        brush.image.tiles.tile = PrRect{center.x - 24, center.y - 24, center.x + 24, center.y + 24};
        brush.image.tiles.x = modes[i % 4];
        brush.image.tiles.y = modes[i % 4];
        brush.image.sampling = PrDrawSampling_Linear;
        if (i >= 4) {
            // Spaced tiles, and the brush turned.
            brush.image.tiles.spacing = PrVec2{12, 6};
            brush.transform = prAffine2Concat(prAffine2Concat(prAffine2Translate(-center), prAffine2Rotate(0.3f)),
                prAffine2Translate(center));
            brush.image.sampling = PrDrawSampling_Nearest;
        }
        prDrawListCornerRect(list, prCornerRectRound(c, 16), {.brush = &brush});
        const char* names[] = {"none", "clamp", "repeat", "reflect"};
        String text = format("{}{}", names[i % 4], i >= 4 ? ", spaced, turned, nearest" : "");
        grid.captionAt(i, text, 20);
    }
}

void nineSlices(PrDrawList* list, float width, float height, double) {
    const PrDrawImage* frame = frameImage();
    Grid grid{width, height, 4, 2};
    PrDrawSliceMode modes[] = {
        PrDrawSliceMode_Stretch, PrDrawSliceMode_Repeat,
        PrDrawSliceMode_Round, PrDrawSliceMode_Space,
    };
    for (uint32_t i = 0; i < 8; ++i) {
        PrRect c = grid.at(i, 20);
        PrDrawNineSlice slices{};
        for (uint32_t k = 0; k < 4; ++k) {
            slices.slices[k] = 12;
            slices.widths[k] = i < 4 ? 24 : 36;
        }
        slices.horizontal = modes[i % 4];
        slices.vertical = modes[i % 4];
        slices.center = i < 4;
        PrRect destination = i < 4 ? inset(c, 10) : PrRect{c.left, c.top + 30, c.right, c.bottom - 30};
        drawNineSlice(list, frame, &slices, destination, PrDrawSampling_Nearest, kWhite);
        const char* names[] = {"stretch", "repeat", "round", "space"};
        String text = format("{}{}", names[i % 4], i < 4 ? ", center" : ", no center, wider");
        grid.captionAt(i, text, 20);
    }
}

// Returns a 60-unit square of a diagonal gradient, of the kind that CSS makes for border-image-source.
PrDrawList* gradientSquare() {
    PrDrawList* list = prDrawListCreate();
    const PrDrawColorStop stops[] = {
        {.offset = 0, .color = kRed},
        {.offset = 0.5f, .color = kGold},
        {.offset = 1, .color = kBlue},
    };
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{60, 60}, stops, 3);
    prDrawListRect(list, PrRect{0, 0, 60, 60}, {.brush = &brush});
    return list;
}

// Pushes a group over the cell whose filter is the single node, and releases the caller's reference.
void pushNodeFilter(PrDrawList* list, PrRect cell, const PrDrawFilterNode& node) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    prDrawFilterBuilderAdd(builder, &node);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = cell;
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawFilterRelease(filter);
}

void listSources(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    PrDrawList* gradient = gradientSquare();
    PrDrawSource square = prDrawSourceList(gradient, PrRect{0, 0, 60, 60});
    // Nine-slices of the gradient, as border-image slices a gradient source.
    PrDrawSliceMode modes[] = {PrDrawSliceMode_Stretch, PrDrawSliceMode_Round};
    for (uint32_t i = 0; i < 2; ++i) {
        PrDrawNineSlice slices{};
        for (uint32_t k = 0; k < 4; ++k) {
            slices.slices[k] = 20;
            slices.widths[k] = 24;
        }
        slices.horizontal = slices.vertical = modes[i];
        prDrawListNineSlice(list, square, &slices, inset(grid.at(i, 20), 20), PrDrawSampling_Linear, {.color = kWhite});
    }
    grid.captionAt(0, "a gradient list nine-sliced, stretched", 20);
    grid.captionAt(1, "the same, round", 20);

    // A list's area drawn as an image, at the scale at which it lands: the motif's center, magnified.
    PrDrawSource center = prDrawSourceList(motif(), PrRect{10, 10, 30, 30});
    PrRect magnified = squareIn(grid.at(2, 20), 200);
    PrDrawBrush centerBrush = prDrawBrushImage(center, magnified, PrDrawSampling_Linear);
    prDrawListRect(list, magnified, {.brush = &centerBrush});
    grid.captionAt(2, "an image brush of a list's area, magnified crisply", 20);

    // A filter's image node over a list, and a tile node repeating the motif.
    PrRect c = grid.at(3, 20);
    PrDrawFilterNode image{};
    image.kind = PrDrawFilterKind_Image;
    image.space = PrColorSpace_Srgb;
    image.image.source = prDrawSourceList(motif(), PrRect{0, 0, 40, 40});
    image.image.rect = squareIn(c, 160);
    image.image.sampling = PrDrawSampling_Linear;
    pushNodeFilter(list, c, image);
    prDrawListPopGroup(list);
    grid.captionAt(3, "a filter's image node drawing a list", 20);

    c = grid.at(4, 20);
    PrDrawFilterNode tile{};
    tile.kind = PrDrawFilterKind_Tile;
    tile.space = PrColorSpace_Srgb;
    tile.inputs[0] = PrDrawFilterInput_Source;
    tile.tile = prRectAt(PrVec2{c.left, c.top}, PrVec2{44, 44});
    pushNodeFilter(list, c, tile);
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{c.left + 2, c.top + 2}));
    prDrawListEmbed(list, motif(), PrDVec2{0, 0});
    prDrawListPopTransform(list);
    prDrawListPopGroup(list);
    grid.captionAt(4, "a tile node repeating the motif", 20);

    // A list brush: the gradient, tiled small.
    c = grid.at(5, 20);
    PrDrawBrush tiles = prDrawBrushDefault(PrDrawBrushKind_Image);
    tiles.image.source = square;
    tiles.image.tiles.tile = prRectAt(PrVec2{c.left, c.top}, PrVec2{30, 30});
    tiles.image.tiles.x = tiles.image.tiles.y = PrDrawExtend_Reflect;
    tiles.image.sampling = PrDrawSampling_Linear;
    prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) / 2, extentOf(c) / 2}, 0, {.brush = &tiles});
    grid.captionAt(5, "an image brush of the list, reflected", 20);
    prDrawListRelease(gradient);
}

void writePixels(PrDrawList* list, float width, float height, double) {
    // Written where the device point says, regardless of the transform and clip in force.
    Grid grid{width, height, 2, 1};
    PrRect c = grid.at(0, 40);
    prDrawListRect(list, c, {.color = kGold});
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{3, 3}));
    prDrawListPushClipRect(list, PrRect{0, 0, 10, 10}, PrDrawEdges_Antialiased);
    prDrawListWritePixels(list, testCard(), PrIVec2{int32_t(c.left) + 20, int32_t(c.top) + 20});
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    c = grid.at(1, 40);
    prDrawListRect(list, c, {.color = kBlue});
    prDrawListWritePixels(list, smallTile(), PrIVec2{int32_t(c.left) + 40, int32_t(c.top) + 40});
    drawImage(list, testCard(), PrRect{0, 0, 64, 64}, PrRect{c.left + 100, c.top + 40, c.left + 164, c.top + 104},
        PrDrawSampling_Nearest, 1);
    grid.captionAt(0, "written under a 3x scale and a clip, which it ignores", 40);
    grid.captionAt(1, "a tile written, beside the card drawn", 40);
}

// Returns the color scaled to `brightness` times its own in linear light, which past 1 is brighter than SDR white.
PrColor brighter(PrColor color, float brightness) {
    float c[3] = {color.r, color.g, color.b};
    for (float& channel : c)
        channel = prColorTransferEncode(PrColorTransfer_Srgb,
            prColorTransferDecode(PrColorTransfer_Srgb, channel) * brightness);
    return PrColor{c[0], c[1], c[2], color.a};
}

// Draws a neon tube along what `draw` records with a paint: a glow blurred in linear light and added to the wall, the
// glass in the tube's color, and a core near white, each brighter than the last.
template <typename Draw>
void tube(PrDrawList* list, PrRect bounds, PrColor hue, float brightness, Draw draw) {
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.space = PrColorSpace_SrgbLinear;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{14, 14};
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    prDrawFilterBuilderAdd(builder, &blur);
    PrDrawGroup glow = prDrawGroupDefault();
    glow.bounds = inset(bounds, -48);
    glow.composite = PrDrawComposite_PlusLighter;
    glow.filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    prDrawListPushGroup(list, &glow);
    prDrawFilterRelease(glow.filter);
    PrDrawStroke wide = strokeOf(12, PrPathJoin_Round, PrPathCap_Round);
    draw(PrDrawPaint{.color = brighter(hue, brightness * 0.8f), .stroke = &wide});
    prDrawListPopGroup(list);
    PrDrawStroke glass = strokeOf(5, PrPathJoin_Round, PrPathCap_Round);
    draw(PrDrawPaint{.color = brighter(hue, brightness), .stroke = &glass});
    PrDrawStroke core = strokeOf(1.5f, PrPathJoin_Round, PrPathCap_Round);
    PrColor hot{0.6f + hue.r * 0.4f, 0.6f + hue.g * 0.4f, 0.6f + hue.b * 0.4f, 1};
    draw(PrDrawPaint{.color = brighter(hot, brightness * 2), .stroke = &core});
}

// Returns a heart within the square.
PrPath* heart(PrRect square) {
    float x = square.left;
    float y = square.top;
    float s = prRectWidth(square);
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{x + s * 0.5f, y + s * 0.92f});
    prPathCubicTo(path, PrVec2{x + s * 0.1f, y + s * 0.62f}, PrVec2{x - s * 0.02f, y + s * 0.3f},
        PrVec2{x + s * 0.22f, y + s * 0.14f});
    prPathCubicTo(path, PrVec2{x + s * 0.36f, y + s * 0.05f}, PrVec2{x + s * 0.48f, y + s * 0.16f},
        PrVec2{x + s * 0.5f, y + s * 0.28f});
    prPathCubicTo(path, PrVec2{x + s * 0.52f, y + s * 0.16f}, PrVec2{x + s * 0.64f, y + s * 0.05f},
        PrVec2{x + s * 0.78f, y + s * 0.14f});
    prPathCubicTo(path, PrVec2{x + s * 1.02f, y + s * 0.3f}, PrVec2{x + s * 0.9f, y + s * 0.62f},
        PrVec2{x + s * 0.5f, y + s * 0.92f});
    prPathClose(path);
    return path;
}

// Returns how bright a tube that flickers is at `time`: steady, except for a stutter late in each four seconds.
float flicker(double time) {
    double phase = prF64Fmod(time, 4.0);
    if (phase < 3.1 || phase > 3.6)
        return 1;
    return prF64Sin(time * 57) > 0.2 ? 0.15f : 1;
}

// Colors far past Display P3, one to a row, each beside the answers it can be checked against: the color as drawn,
// which the application maps for its display as it presents; CSS Color 4's mapping for the display that the drawing is
// drawn for, up to its headroom, and into sRGB, computed here and drawn as colors those hold; and each channel clipped
// to sRGB. A color past P3 by a negative channel keeps less chroma wherever it maps. Rec. 2020's red and magenta are
// past P3 by a channel above SDR white, which a display's headroom can show, so they keep more chroma in HDR than in
// SDR.
void gamutMapping(PrDrawList* list, float width, float height, double) {
    prDrawListRect(list, PrRect{0, 0, width, height}, {.color = PrColor{0.12f, 0.12f, 0.13f, 1}});
    struct Sample {
        const char* name;
        PrColorSpace space;
        double channels[3];
    };
    const Sample samples[] = {
        {"Rec. 2020 green", PrColorSpace_Rec2020, {0, 1, 0}},
        {"Rec. 2020 blue", PrColorSpace_Rec2020, {0, 0, 1}},
        {"Rec. 2020 cyan", PrColorSpace_Rec2020, {0, 1, 1}},
        {"deep red", PrColorSpace_Oklch, {0.6, 0.4, 25}},
        {"Rec. 2020 red", PrColorSpace_Rec2020, {1, 0, 0}},
        {"Rec. 2020 magenta", PrColorSpace_Rec2020, {1, 0, 1}},
    };
    // The display that the drawing is drawn for, as the application maps its frames for it.
    const PrAppDisplay& display = drawingDisplay();
    float headroom = display.headroom > 1 ? display.headroom : 1;
    String mappedName = format("mapped for this display: {}, {:g}x", prColorSpaceName(display.gamut), double(headroom));
    const PrStringView columns[] = {"drawn", mappedName, "mapped to sRGB", "clipped to sRGB"};
    constexpr uint32_t kRows = 6;
    constexpr uint32_t kColumns = 4;
    float left = 130;
    float top = 40;
    float cellWidth = (width - left - 20) / kColumns;
    float cellHeight = (height - top - 20) / kRows;
    for (uint32_t c = 0; c < kColumns; ++c)
        label(columns[c], PrVec2{left + cellWidth * float(c) + 6, top - 12});
    for (uint32_t r = 0; r < kRows; ++r) {
        double srgb[3];
        prColorConvert(samples[r].space, PrColorSpace_Srgb, samples[r].channels, srgb);
        PrColor color{float(srgb[0]), float(srgb[1]), float(srgb[2]), 1};
        auto clip = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
        PrColor mapped = prColorGamutMap(prColorToneMap(color, headroom), display.gamut, headroom);
        const PrColor cells[kColumns] = {
            color, mapped, prColorGamutMap(color, PrColorSpace_Srgb, 1),
            PrColor{clip(color.r), clip(color.g), clip(color.b), 1},
        };
        float y = top + cellHeight * float(r);
        label(samples[r].name, PrVec2{20, y + cellHeight / 2 + 4});
        for (uint32_t c = 0; c < kColumns; ++c) {
            float x = left + cellWidth * float(c);
            prDrawListRect(list, PrRect{x + 4, y + 4, x + cellWidth - 4, y + cellHeight - 4}, {.color = cells[c]});
        }
    }
}

void neon(PrDrawList* list, float width, float height, double time) {
    // A dark wall, lit a little around the sign.
    prDrawListRect(list, PrRect{0, 0, width, height}, {.color = PrColor{0.025f, 0.025f, 0.035f, 1}});
    const PrColor pink{1, 0.18f, 0.55f, 1};
    const PrColor cyan{0.15f, 0.85f, 1, 1};
    float signHeight = height * 0.52f;
    PrDrawColorStop light[] = {
        {.offset = 0, .color = PrColor{0.25f, 0.1f, 0.3f, 1}},
        {.offset = 1, .color = PrColor{0.25f, 0.1f, 0.3f, 0}},
    };
    PrDrawBrush wash = prDrawBrushRadial(PrVec2{width / 2, signHeight / 2}, 0, PrVec2{width / 2, signHeight / 2},
        width * 0.45f, light, 2);
    prDrawListRect(list, PrRect{0, 0, width, signHeight}, {.brush = &wash});

    // The word, in outlined letters, and a heart that flickers.
    TextStyle style;
    PrFontVariation thin{PR_FONT_TAG('w', 'g', 'h', 't'), 200};
    style.font = fontVaried("NotoSans.ttf", &thin, 1);
    style.size = signHeight * 0.55f;
    float advance = textAdvance(style, "neon");
    float heartSize = signHeight * 0.5f;
    float left = (width - advance - heartSize - 40) / 2;
    float baseline = signHeight * 0.68f;
    tube(list, PrRect{left, baseline - style.size * 0.6f, left + advance, baseline}, cyan, 2, [&](PrDrawPaint paint) {
        TextStyle stroked = style;
        stroked.paint = paint;
        drawText(list, stroked, "neon", PrVec2{left, baseline});
    });
    PrRect square{left + advance + 40, baseline - heartSize * 0.95f, left + advance + 40 + heartSize, baseline};
    PrPath* shape = heart(square);
    tube(list, square, pink, 3 * flicker(time),
        [&](PrDrawPaint paint) { prDrawListPath(list, shape, PrPathFillRule_NonZero, paint); });
    prPathDestroy(shape);

    // A ramp from SDR white to sixteen times it, in doublings. A display shows it flat past its headroom.
    float top = signHeight + 30;
    PrRect ramp{40, top, width - 40, top + 36};
    PrDrawColorStop stops[5];
    for (uint32_t i = 0; i < 5; ++i)
        stops[i] = PrDrawColorStop{.offset = float(i) / 4, .color = brighter(kWhite, float(1u << i))};
    PrDrawBrush brightness = prDrawBrushLinear(PrVec2{ramp.left, 0}, PrVec2{ramp.right, 0}, stops, 5);
    prDrawListRect(list, ramp, {.brush = &brightness});
    PrDrawStroke hairline = hairlineStroke();
    for (uint32_t i = 0; i < 5; ++i) {
        float x = ramp.left + prRectWidth(ramp) * float(i) / 4;
        PrPath* tick = prPathCreate();
        prPathMoveTo(tick, PrVec2{x, ramp.bottom});
        prPathLineTo(tick, PrVec2{x, ramp.bottom + 6});
        prDrawListPath(list, tick, PrPathFillRule_NonZero, {.color = kGray, .stroke = &hairline});
        prPathDestroy(tick);
        String text = format("{}x white", 1u << i);
        label(text, PrVec2{x - (i == 4 ? 48.0f : i == 0 ? 0.0f : 20.0f), ramp.bottom + 20});
    }

    // Primaries of wider gamuts, each beside its sRGB clip.
    struct Gamut {
        PrColorSpace space;
        const char* name;
    };
    const Gamut gamuts[] = {
        {PrColorSpace_Srgb, "sRGB"}, {PrColorSpace_DisplayP3, "Display P3"},
        {PrColorSpace_Rec2020, "Rec. 2020"},
    };
    float rowTop = ramp.bottom + 44;
    float rowHeight = (height - rowTop - 20) / 3;
    for (uint32_t g = 0; g < 3; ++g) {
        float y = rowTop + rowHeight * float(g);
        label(gamuts[g].name, PrVec2{40, y + rowHeight / 2 + 4});
        for (uint32_t c = 0; c < 3; ++c) {
            double in[3] = {c == 0 ? 1.0 : 0.0, c == 1 ? 1.0 : 0.0, c == 2 ? 1.0 : 0.0};
            double out[3];
            prColorConvert(gamuts[g].space, PrColorSpace_Srgb, in, out);
            PrColor wide{float(out[0]), float(out[1]), float(out[2]), 1};
            PrColor clipped{clamp(wide.r, 0.0f, 1.0f), clamp(wide.g, 0.0f, 1.0f), clamp(wide.b, 0.0f, 1.0f), 1};
            float x = 160 + float(c) * (width - 200) / 3;
            float w = (width - 200) / 3 - 16;
            PrRect swatch{x, y + 4, x + w, y + rowHeight - 4};
            prDrawListRect(list, PrRect{swatch.left, swatch.top, (swatch.left + swatch.right) / 2, swatch.bottom},
                {.color = wide});
            prDrawListRect(list, PrRect{(swatch.left + swatch.right) / 2, swatch.top, swatch.right, swatch.bottom},
                {.color = clipped});
        }
    }
}

} // namespace

const Array<Drawing>& paintFeatures() {
    static const Array<Drawing> features({
        {
            "Linear gradients", "Pad, reflect, repeat and none, stops and hints, transparency, tints and strokes.",
            linearGradients,
        },
        {"Radial gradients", "Spreads, focal points, two-circle cones and object bounds.", radialGradients},
        {"Conic and diamond gradients", "Conic sweeps and hard stops, and diamonds by spread.", conicAndDiamond},
        {
            "Interpolation spaces",
            "Blue to yellow in each space, then red to cyan by each hue direction.", interpolation,
        },
        {"Brush spaces", "Brushes in user space, object bounds and target space.", brushSpaces},
        {"Images", "Sampling up and down, transforms, tints, source rectangles and brushes.", images},
        {"Image brushes", "Each tile mode about a 48-pixel tile, plain, then spaced and turned.", imageBrushes},
        {"Nine-slices", "Each slice mode, with the center and without it.", nineSlices},
        {"List sources", "Lists drawn where images are: nine-slices, image brushes and filter nodes.", listSources},
        {"Write pixels", "Pixels replacing the target's at a device point.", writePixels},
        {
            "Neon", "Tubes past SDR white that glow and flicker, a ramp to sixteen times white, and primaries of wider "
                "gamuts beside their sRGB clips. Press H for HDR.", neon,
        },
        {
            "Gamut mapping",
            "Colors far past Display P3, beside CSS's mapping for this display and into sRGB, and a clip to sRGB. "
                "With H, the first column matches the second; in SDR, it matches the clip.",
            gamutMapping,
        },
    });
    return features;
}

} // namespace drawings
