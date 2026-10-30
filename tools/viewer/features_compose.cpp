// Feature drawings for clips, compositing, groups, masks and lists.

#include "tools/viewer/features_compose.h"

#include "tools/viewer/drawing.h"

namespace viewer {
namespace {

constexpr PrDrawColorStop kSunset[] = {
    {.offset = 0, .color = PrColor{0.95f, 0.75f, 0.2f, 1}},
    {.offset = 0.5f, .color = PrColor{0.9f, 0.3f, 0.35f, 1}},
    {.offset = 1, .color = PrColor{0.25f, 0.2f, 0.6f, 1}},
};

// Draws diagonal stripes over the rectangle, to show what shows through.
void stripes(PrDrawList* list, PrRect c, PrColor color) {
    prDrawListPushClipRect(list, c, PrDrawEdges_Antialiased);
    for (float x = c.left - 200; x < c.right + 200; x += 24) {
        PrVec2 band[] = {{x, c.top}, {x + 12, c.top}, {x + 12 + 200, c.bottom}, {x + 200, c.bottom}};
        prDrawListPolylineContour(list, band, 4, PrDrawContour_Closed, PrPathFillRule_NonZero, {.color = color});
    }
    prDrawListPopClip(list);
}

void clips(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    PrRect c = grid.at(0, 30);
    frameCell(list, c);
    prDrawListPushClipRect(list, PrRect{c.left + 20, c.top + 60, c.right - 20, c.bottom - 60}, PrDrawEdges_Antialiased);
    stripes(list, c, kBlue);
    prDrawListPopClip(list);

    c = grid.at(1, 30);
    frameCell(list, c);
    PrVec2 center = prRectCenter(c);
    float r = extentOf(c) / 2 - 10;
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, center, PrVec2{r, r}, 0);
    prDrawListPushClipPath(list, circle, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prPathDestroy(circle);
    PrPath* shape = star(center, r * 1.1f, 5, 2);
    prDrawListPushClipPath(list, shape, PrPathFillRule_EvenOdd, PrDrawEdges_Antialiased);
    prPathDestroy(shape);
    stripes(list, c, kBlue);
    prDrawListPopClip(list);
    prDrawListPopClip(list);

    // A clip recorded in a rotated space, with stripes drawn back in the drawing's space through the inverse. Every
    // push nests inside the previous one.
    c = grid.at(2, 30);
    frameCell(list, c);
    center = prRectCenter(c);
    PrAffine2 rotated = prAffine2Concat(prAffine2Rotate(0.3f), prAffine2Translate(center));
    PrAffine2 back = prAffine2Identity();
    prAffine2Invert(rotated, &back);
    prDrawListPushTransformAffine2(list, rotated);
    prDrawListPushClipRect(list, PrRect{-r * 0.7f, -r * 0.7f, r * 0.7f, r * 0.7f}, PrDrawEdges_Aliased);
    prDrawListPushTransformAffine2(list, back);
    stripes(list, c, kBlue);
    prDrawListPopTransform(list);
    prDrawListPopClip(list);
    prDrawListPopTransform(list);

    // A target-pixel clip that ignores the rotation in force.
    c = grid.at(3, 30);
    frameCell(list, c);
    center = prRectCenter(c);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.6f), prAffine2Translate(center)));
    prDrawListPushClipTargetRect(list, PrRect{
        c.left + 30, c.top + 30, c.right - 30,
        c.bottom - 30,
    }, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{-200, -200, 200, 200}, {.color = kGreen});
    stripes(list, PrRect{-200, -200, 200, 200}, kGold);
    prDrawListPopClip(list);
    prDrawListPopTransform(list);

    // Shaped corners, and the same shape inset as a padding box clips.
    c = grid.at(4, 30);
    frameCell(list, c);
    prDrawListPushClipCornerRect(
        list, PrCornerRect{inset(c, 10), {cornerOf(70, 2), cornerOf(30, -1), cornerOf(60, 1), cornerOf(40, 0)}},
        PrDrawEdges_Antialiased);
    stripes(list, c, kPurple);
    prDrawListPopClip(list);

    c = grid.at(5, 30);
    frameCell(list, c);
    PrCornerRect rounds{inset(c, 10), {cornerOf(80, 1), cornerOf(80, 1), cornerOf(80, 2), cornerOf(80, 2)}};
    const float insets[4] = {10, 30, 20, 50};
    prDrawListCornerRect(list, rounds, {.color = kGold});
    prDrawListPushClipCornerRect(list, prCornerRectInset(rounds, insets), PrDrawEdges_Antialiased);
    stripes(list, c, kTeal);
    prDrawListPopClip(list);
    const char* captions[] = {
        "rect",
        "circle, then an evenodd star",
        "aliased rect in a rotated space",
        "target rect under a rotation",
        "shaped corners",
        "padding box, inset 10 30 20 50 from gold",
    };
    for (uint32_t i = 0; i < 6; ++i)
        grid.captionAt(i, captions[i], 30);
}

// Draws a cell that shows an operator: the blue destination, then the red source composited by the operator where it
// covers, inside an isolated group so that it composites over the blue alone.
void operatorCell(PrDrawList* list, PrRect c, PrDrawComposite op, PrDrawBlendMode blend) {
    prDrawListRect(list, c, {.color = PrColor{0.85f, 0.85f, 0.85f, 1}});
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = c;
    group.isolated = true;
    prDrawListPushGroup(list, &group);
    float r = extentOf(c) * 0.3f;
    PrVec2 center = prRectCenter(c);
    prDrawListEllipse(list, PrVec2{center.x - r * 0.4f, center.y - r * 0.4f}, PrVec2{r, r}, 0,
        {.color = withAlpha(kBlue, 0.9f)});
    prDrawListRect(list, PrRect{
        center.x - r * 0.2f, center.y - r * 0.2f, center.x + r * 1.2f,
        center.y + r * 1.2f,
    }, {.color = withAlpha(kRed, 0.9f), .composite = op, .blendMode = blend});
    prDrawListPopGroup(list);
}

void composites(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 5, 3};
    const char* names[] = {
        "source-over",      "clear",          "copy",
        "source-in",        "source-out",     "source-atop",
        "destination-over", "destination-in", "destination-out",
        "destination-atop", "xor",            "plus-lighter",
        "plus-darker",
    };
    for (uint32_t op = 0; op < PrDrawComposite_Count; ++op) {
        operatorCell(list, grid.at(op, 10), PrDrawComposite(op), PrDrawBlendMode_Normal);
        grid.captionAt(op, names[op], 10);
    }
}

void blendModes(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 5, 4};
    for (uint32_t mode = 0; mode < PrDrawBlendMode_Count; ++mode) {
        PrRect c = grid.at(mode, 8);
        PrDrawBrush back = prDrawBrushLinear(PrVec2{c.left, 0}, PrVec2{c.right, 0}, kSunset, 3);
        prDrawListRect(list, c, {.brush = &back});
        const PrDrawColorStop cool[] = {
            {.offset = 0, .color = PrColor{0.1f, 0.7f, 0.9f, 1}},
            {.offset = 1, .color = PrColor{0.2f, 0.9f, 0.3f, 1}},
        };
        PrDrawBrush front = prDrawBrushLinear(PrVec2{0, c.top}, PrVec2{0, c.bottom}, cool, 2);
        prDrawListEllipse(list, prRectCenter(c), PrVec2{extentOf(c) * 0.4f, extentOf(c) * 0.4f}, 0,
            {.brush = &front, .blendMode = PrDrawBlendMode(mode)});
        const char* names[] = {
            "normal",     "multiply",   "screen",     "overlay",
            "darken",     "lighten",    "color-dodge", "color-burn",
            "hard-light", "soft-light", "difference", "exclusion",
            "hard-mix",   "hue",        "saturation", "color",
            "luminosity",
        };
        grid.captionAt(mode, names[mode], 8);
    }
}

void threeCircles(PrDrawList* list, PrRect c, float alpha, PrDrawBlendMode blendMode = PrDrawBlendMode_Normal) {
    PrVec2 center = prRectCenter(c);
    float r = extentOf(c) * 0.25f;
    PrColor colors[] = {kRed, kGreen, kBlue};
    for (uint32_t i = 0; i < 3; ++i) {
        float angle = -kPi / 2 + 2 * kPi * float(i) / 3;
        PrVec2 p{center.x + r * 0.6f * std::cos(angle), center.y + r * 0.6f * std::sin(angle)};
        prDrawListEllipse(list, p, PrVec2{r, r}, 0, {.color = withAlpha(colors[i], alpha), .blendMode = blendMode});
    }
}

void groups(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    for (uint32_t i = 0; i < 8; ++i)
        stripes(list, grid.at(i, 10), withAlpha(kInk, 0.15f));
    // Translucent shapes against the same shapes opaque in a translucent group.
    threeCircles(list, grid.at(0), 0.5f);
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    threeCircles(list, grid.at(1), 1);
    prDrawListPopGroup(list);
    // Multiply reaching the stripes beneath, and kept inside an isolated group.
    PrRect c = grid.at(2);
    threeCircles(list, c, 1, PrDrawBlendMode_Multiply);
    group = prDrawGroupDefault();
    group.isolated = true;
    prDrawListPushGroup(list, &group);
    threeCircles(list, grid.at(3), 1, PrDrawBlendMode_Multiply);
    prDrawListPopGroup(list);
    // Knockout: each translucent circle replaces those before it.
    group = prDrawGroupDefault();
    group.knockout = true;
    prDrawListPushGroup(list, &group);
    threeCircles(list, grid.at(4), 0.6f);
    prDrawListPopGroup(list);
    // A group blended as a whole, and a group composited destination-out to punch its shape.
    group = prDrawGroupDefault();
    group.blendMode = PrDrawBlendMode_Difference;
    prDrawListPushGroup(list, &group);
    threeCircles(list, grid.at(5), 1);
    prDrawListPopGroup(list);
    c = grid.at(6);
    PrDrawGroup isolated = prDrawGroupDefault();
    isolated.isolated = true;
    prDrawListPushGroup(list, &isolated);
    prDrawListRect(list, inset(c, 20), {.color = kGold});
    group = prDrawGroupDefault();
    group.opacity = 0.8f;
    group.composite = PrDrawComposite_DestinationOut;
    prDrawListPushGroup(list, &group);
    threeCircles(list, c, 1);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    // A group bounded to part of what it holds.
    c = grid.at(7);
    group = prDrawGroupDefault();
    group.opacity = 0.9f;
    group.bounds = PrRect{c.left, c.top, (c.left + c.right) / 2, c.bottom};
    prDrawListPushGroup(list, &group);
    threeCircles(list, c, 1);
    prDrawListPopGroup(list);
    const char* captions[] = {
        "translucent circles",
        "opaque circles, a group at half opacity",
        "multiply reaching the stripes",
        "multiply kept to an isolated group",
        "knockout",
        "a group blended by difference",
        "destination-out through gold",
        "a group bounded to its left half",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i], 10);
}

void masks(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 3, 2};
    const PrDrawImage* card = testCard();
    // An alpha mask fading left to right, and its inversion.
    for (uint32_t i = 0; i < 2; ++i) {
        PrRect c = grid.at(i, 20);
        stripes(list, c, withAlpha(kInk, 0.15f));
        const PrDrawColorStop fade[] = {
            {.offset = 0, .color = withAlpha(kWhite, 1)},
            {.offset = 1, .color = withAlpha(kWhite, 0)},
        };
        PrDrawBrush brush = prDrawBrushLinear(PrVec2{c.left, 0}, PrVec2{c.right, 0}, fade, 2);
        PrDrawMask mask = prDrawMaskDefault(&brush);
        mask.invert = i == 1;
        PrDrawGroup group = prDrawGroupDefault();
        group.mask = &mask;
        prDrawListPushGroup(list, &group);
        drawImage(list, card, PrRect{0, 0, 64, 64}, c, PrDrawSampling_Linear, 1);
        prDrawListPopGroup(list);
    }
    // A luminance mask of a radial gradient, in sRGB and in linear light.
    for (uint32_t i = 0; i < 2; ++i) {
        PrRect c = grid.at(2 + i, 20);
        const PrDrawColorStop glow[] = {{.offset = 0, .color = kWhite}, {.offset = 1, .color = PrColor{0, 0, 0, 1}}};
        PrDrawBrush brush = prDrawBrushRadial(prRectCenter(c), 0, prRectCenter(c), extentOf(c) / 2, glow, 2);
        PrDrawMask mask = prDrawMaskDefault(&brush);
        mask.mode = PrDrawMaskMode_Luminance;
        mask.space = i == 0 ? PrColorSpace_Srgb : PrColorSpace_SrgbLinear;
        PrDrawGroup group = prDrawGroupDefault();
        group.mask = &mask;
        prDrawListPushGroup(list, &group);
        prDrawListRect(list, c, {.color = kBlue});
        prDrawListRect(list, inset(c, 40), {.color = kGold});
        prDrawListPopGroup(list);
    }
    // A list as the mask: the motif's alpha, tiled.
    PrRect c = grid.at(4, 20);
    PrDrawSource source = prDrawSourceList(motif(), PrRect{0, 0, 40, 40});
    PrRect tile{c.left, c.top, c.left + 60, c.top + 60};
    PrDrawBrush tiled = prDrawBrushImage(source, tile, PrDrawSampling_Linear);
    tiled.image.tiles.x = PrDrawExtend_Repeat;
    tiled.image.tiles.y = PrDrawExtend_Repeat;
    PrDrawMask mask = prDrawMaskDefault(&tiled);
    PrDrawGroup group = prDrawGroupDefault();
    group.mask = &mask;
    prDrawListPushGroup(list, &group);
    PrDrawBrush sunset = prDrawBrushLinear(PrVec2{c.left, c.top}, PrVec2{c.right, c.bottom}, kSunset, 3);
    prDrawListRect(list, c, {.brush = &sunset});
    prDrawListPopGroup(list);
    // A mask with a translucent tint.
    c = grid.at(5, 20);
    const PrDrawColorStop bars[] = {
        {.offset = 0, .color = kWhite},
        {.offset = 0.5f, .color = kWhite},
        {.offset = 0.5f, .color = withAlpha(kWhite, 0)},
        {.offset = 1, .color = withAlpha(kWhite, 0)},
    };
    PrDrawBrush repeating = prDrawBrushLinear(PrVec2{c.left, 0}, PrVec2{c.left + 40, 0}, bars, 4);
    repeating.extend = PrDrawExtend_Repeat;
    mask = prDrawMaskDefault(&repeating);
    mask.brush.opacity = 0.6f;
    group = prDrawGroupDefault();
    group.mask = &mask;
    prDrawListPushGroup(list, &group);
    threeCircles(list, c, 1);
    prDrawListPopGroup(list);
    const char* captions[] = {
        "alpha mask fading right", "the same inverted",
        "luminance of a radial, sRGB", "the same in linear light",
        "a list tiled as the mask", "repeating bars, 60% tint",
    };
    for (uint32_t i = 0; i < 6; ++i)
        grid.captionAt(i, captions[i], 20);
}

void lists(PrDrawList* list, float width, float height, double) {
    PrDrawList* drawn = motif();
    // The motif played many times, each under its own transform.
    for (uint32_t i = 0; i < 24; ++i) {
        float angle = 2 * kPi * float(i) / 24;
        float s = 0.6f + 0.05f * float(i % 6);
        PrAffine2 t = prAffine2Concat(prAffine2Concat(prAffine2Translate(PrVec2{-20, -20}), prAffine2Scale(PrVec2{s * 2, s * 2})),
            prAffine2Concat(prAffine2Rotate(angle), prAffine2Translate(PrVec2{
                width * 0.25f + 180 * std::cos(angle),
                height * 0.5f + 180 * std::sin(angle),
            })));
        prDrawListPushTransformAffine2(list, t);
        prDrawListEmbed(list, drawn, PrDVec2{0, 0});
        prDrawListPopTransform(list);
    }
    PrVec2 corner{width * 0.25f - 80, height * 0.5f - 80};
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Scale(PrVec2{4, 4}), prAffine2Translate(corner)));
    prDrawListEmbed(list, drawn, PrDVec2{0, 0});
    prDrawListPopTransform(list);
    // The motif as a brush: tiles repeating, and reflecting in a rotated space.
    PrRect c{width * 0.55f, 40, width - 40, height / 2 - 20};
    PrDrawBrush brush = prDrawBrushDefault(PrDrawBrushKind_Image);
    brush.image.source = prDrawSourceList(drawn, PrRect{0, 0, 40, 40});
    brush.image.tiles.tile = prRectAt(PrVec2{c.left, c.top}, PrVec2{50, 50});
    brush.image.tiles.spacing = PrVec2{10, 10};
    brush.image.tiles.x = PrDrawExtend_Repeat;
    brush.image.tiles.y = PrDrawExtend_Repeat;
    brush.image.sampling = PrDrawSampling_Linear;
    prDrawListCornerRect(list, prCornerRectRound(c, 30), {.brush = &brush});
    c = PrRect{width * 0.55f, height / 2 + 20, width - 40, height - 40};
    brush.image.tiles.tile = PrRect{0, 0, 80, 80};
    brush.image.tiles.spacing = PrVec2{0, 0};
    brush.image.tiles.x = PrDrawExtend_Reflect;
    brush.image.tiles.y = PrDrawExtend_Reflect;
    brush.transform = prAffine2Concat(prAffine2Rotate(0.25f), prAffine2Translate(PrVec2{c.left, c.top}));
    prDrawListEllipse(list, prRectCenter(c), PrVec2{prRectWidth(c) / 2, prRectHeight(c) / 2}, 0, {.brush = &brush});
    caption(PrRect{0, height - 30, width * 0.5f, height - 12}, "one list embedded in a ring, and at 4x");
    caption(PrRect{width * 0.55f, height / 2 - 18, width - 40, height / 2}, "as a brush: spaced repeats");
    caption(PrRect{width * 0.55f, height - 36, width - 40, height - 18}, "as a brush: rotated reflections");
}

} // namespace

const std::vector<Drawing>& composeFeatures() {
    static const std::vector<Drawing> features = {
        {"Clips", "Clips of each kind, nested and under transforms.", clips},
        {
            "Composite operators",
            "The red square onto the blue circle by each operator, inside an isolated group.",
            composites,
        },
        {"Blend modes", "A cool gradient circle over a warm gradient by each blend mode.", blendModes},
        {"Groups", "Opacity, isolation, knockout, blending, operators and bounds on groups.", groups},
        {"Masks", "Alpha and luminance masks from gradients and lists.", masks},
        {"Embedded lists", "One list embedded under many transforms, and as a brush.", lists},
    };
    return features;
}

} // namespace viewer
