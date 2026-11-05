// Feature drawings for boxes: borders, shadows and decorations.

#include "examples/draw_features/features_boxes.h"

#include "pr/string.h"

#include "examples/draw_features/drawing.h"

namespace drawings {

using namespace pr;

namespace {

PrDrawBorder borderOf(float width, PrDrawBorderStyle style, PrColor color, float radius) {
    PrDrawBorder border{};
    for (uint32_t i = 0; i < 4; ++i) {
        border.shape.corners[i] = cornerOf(radius, 1);
        border.sides[i] = PrDrawBorderSide{width, style, color, nullptr, 0};
    }
    return border;
}

void borderStyles(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    PrDrawBorderStyle styles[] = {
        PrDrawBorderStyle_Solid,  PrDrawBorderStyle_Double,
        PrDrawBorderStyle_Dashed, PrDrawBorderStyle_Dotted,
        PrDrawBorderStyle_Groove, PrDrawBorderStyle_Ridge,
        PrDrawBorderStyle_Inset,  PrDrawBorderStyle_Outset,
    };
    for (uint32_t i = 0; i < 8; ++i) {
        PrRect c = grid.at(i, 30);
        PrDrawBorder border = borderOf(18, styles[i], i < 4 ? kBlue : PrColor{0.4f, 0.6f, 0.9f, 1}, i % 2 == 0 ? 0 : 36);
        border.shape.rect = c;
        prDrawListBorder(list, &border, {});
        const char* names[] = {"solid", "double", "dashed", "dotted", "groove", "ridge", "inset",  "outset"};
        grid.captionAt(i, names[i], 30);
    }
}

void borderSides(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    // Sides of different widths and colors, square and round.
    PrColor colors[] = {kRed, kGreen, kBlue, kGold};
    float widthsOf[] = {10, 30, 20, 4};
    for (uint32_t k = 0; k < 2; ++k) {
        PrDrawBorder border{};
        for (uint32_t i = 0; i < 4; ++i) {
            border.shape.corners[i] = cornerOf(k == 0 ? 0 : 50, 1);
            border.sides[i] = PrDrawBorderSide{widthsOf[i], PrDrawBorderStyle_Solid, colors[i], nullptr, 0};
        }
        border.shape.rect = grid.at(k, 30);
        prDrawListBorder(list, &border, {});
    }
    // Shaped corners: squircle, bevel, scoop, notch.
    PrDrawBorder shaped = borderOf(12, PrDrawBorderStyle_Solid, kPurple, 0);
    shaped.shape.corners[0] = cornerOf(50, 2);
    shaped.shape.corners[1] = cornerOf(40, 0);
    shaped.shape.corners[2] = cornerOf(40, -1);
    shaped.shape.corners[3] = cornerOf(30, -kPrF32Infinity);
    shaped.shape.rect = grid.at(2, 30);
    prDrawListBorder(list, &shaped, {});
    // Styles mixed around one box.
    PrDrawBorder mixed = borderOf(16, PrDrawBorderStyle_Solid, kTeal, 30);
    mixed.sides[1].style = PrDrawBorderStyle_Dashed;
    mixed.sides[2].style = PrDrawBorderStyle_Double;
    mixed.sides[3].style = PrDrawBorderStyle_Dotted;
    mixed.sides[3].color = kRed;
    mixed.shape.rect = grid.at(3, 30);
    prDrawListBorder(list, &mixed, {});
    // Dashes and dots around rounded corners, a fixed count, and elliptical radii.
    PrDrawBorder dashed = borderOf(10, PrDrawBorderStyle_Dashed, kInk, 50);
    dashed.shape.rect = grid.at(4, 30);
    prDrawListBorder(list, &dashed, {});
    PrDrawBorder dotted = borderOf(12, PrDrawBorderStyle_Dotted, kRed, 60);
    dotted.shape.rect = grid.at(5, 30);
    prDrawListBorder(list, &dotted, {});
    PrDrawBorder counted = borderOf(8, PrDrawBorderStyle_Dashed, kBlue, 0);
    for (PrDrawBorderSide& side : counted.sides)
        side.patternPeriods = 3;
    counted.shape.rect = grid.at(6, 30);
    prDrawListBorder(list, &counted, {});
    // A gradient brush on each side, and a border thicker than its box.
    PrRect c = grid.at(7, 30);
    const PrDrawColorStop stops[] = {{.offset = 0, .color = kGold}, {.offset = 1, .color = kPurple}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{c.left, c.top}, PrVec2{c.right, c.bottom}, stops, 2);
    PrDrawBorder brushed = borderOf(22, PrDrawBorderStyle_Solid, kWhite, 40);
    brushed.shape.corners[1] = PrCorner{{80, 30}, 1};
    for (PrDrawBorderSide& side : brushed.sides)
        side.brush = &brush;
    brushed.sides[2].width = 70;
    brushed.shape.rect = c;
    prDrawListBorder(list, &brushed, {});
    const char* captions[] = {
        "sides 10 30 20 4, square",
        "the same, round",
        "squircle, bevel, scoop, notch",
        "styles mixed",
        "dashes around round corners",
        "dots around round corners",
        "three dashes a side",
        "gradient sides, bottom past the box",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i], 30);
}

PrDrawShadow shadowOf(PrRect rect, float radius, PrVec2 offset, float blur, float spread) {
    PrDrawShadow shadow{};
    shadow.shape.rect = rect;
    for (PrCorner& corner : shadow.shape.corners)
        corner = cornerOf(radius, 1);
    shadow.offset = offset;
    shadow.sigma = blur / 2;
    shadow.spread = spread;
    return shadow;
}

void shadows(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    PrColor shade = withAlpha(kInk, 0.6f);
    struct Case {
        float radius;
        PrVec2 offset;
        float blur;
        float spread;
    };
    const Case drops[] = {{0, {8, 8}, 0, 0}, {16, {6, 10}, 12, 0}, {40, {0, 0}, 40, 0}, {24, {12, 12}, 20, 16}};
    for (uint32_t i = 0; i < 4; ++i) {
        PrRect c = inset(grid.at(i, 20), 50);
        PrDrawShadow shadow = shadowOf(c, drops[i].radius, drops[i].offset, drops[i].blur, drops[i].spread);
        prDrawListDropShadow(list, &shadow, {.color = shade});
        prDrawListCornerRect(list, prCornerRectRound(c, drops[i].radius), {.color = kWhite});
    }
    // A translucent caster shows what lies behind it, not its shadow.
    PrRect c = inset(grid.at(4, 20), 50);
    for (float x = c.left - 40; x < c.right + 40; x += 20)
        prDrawListRect(list, PrRect{x, c.top - 40, x + 10, c.bottom + 40}, {.color = withAlpha(kGold, 0.6f)});
    PrDrawShadow shadow = shadowOf(c, 20, PrVec2{10, 10}, 24, 0);
    prDrawListDropShadow(list, &shadow, {.color = withAlpha(kBlue, 0.8f)});
    prDrawListCornerRect(list, prCornerRectRound(c, 20), {.color = withAlpha(kWhite, 0.3f)});
    // Inner shadows.
    c = inset(grid.at(5, 20), 40);
    prDrawListCornerRect(list, prCornerRectRound(c, 30), {.color = kWhite});
    shadow = shadowOf(c, 30, PrVec2{8, 8}, 20, 0);
    prDrawListInnerShadow(list, &shadow, {.color = shade});
    c = inset(grid.at(6, 20), 40);
    prDrawListRect(list, c, {.color = kGold});
    shadow = shadowOf(c, 0, PrVec2{0, 0}, 30, 10);
    prDrawListInnerShadow(list, &shadow, {.color = withAlpha(kRed, 0.9f)});
    // Under a scale of 2, shadows that scale and one measured in target pixels.
    c = grid.at(7, 20);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Scale(PrVec2{2, 2}),
        prAffine2Translate(PrVec2{c.left + 20, c.top + 20})));
    shadow = shadowOf(PrRect{0, 0, 40, 40}, 8, PrVec2{4, 4}, 8, 0);
    prDrawListDropShadow(list, &shadow, {.color = shade});
    prDrawListCornerRect(list, prCornerRectRound(shadow.shape.rect, 8), {.color = kWhite});
    shadow = shadowOf(PrRect{50, 50, 90, 90}, 8, PrVec2{4, 4}, 8, 0);
    shadow.nonScaling = true;
    prDrawListDropShadow(list, &shadow, {.color = shade});
    prDrawListCornerRect(list, prCornerRectRound(shadow.shape.rect, 8), {.color = kWhite});
    prDrawListPopTransform(list);
    const char* captions[] = {
        "hard offset",
        "blur 12",
        "blur 40, centered",
        "spread 16",
        "translucent caster over stripes",
        "inner, offset",
        "inner, spread",
        "at 2x: scaling, then non-scaling",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i], 20);
}

void decorations(PrDrawList* list, float width, float height, double) {
    PrDrawDecorationStyle styles[] = {
        PrDrawDecorationStyle_Solid, PrDrawDecorationStyle_Double,
        PrDrawDecorationStyle_Dotted, PrDrawDecorationStyle_Dashed,
        PrDrawDecorationStyle_Wavy,
    };
    float thicknesses[] = {1, 2, 4};
    float y = 60;
    for (uint32_t s = 0; s < 5; ++s) {
        for (uint32_t t = 0; t < 3; ++t) {
            PrDrawDecoration d{};
            d.origin = PrVec2{60 + float(t) * (width - 120) / 3, y};
            d.length = (width - 120) / 3 - 40;
            d.thickness = thicknesses[t];
            d.style = styles[s];
            d.wavelength = d.thickness * 8 + 4;
            d.amplitude = d.thickness * 1.5f + 1;
            prDrawListDecoration(list, &d, {.color = kInk});
            const char* names[] = {"solid", "double", "dotted", "dashed", "wavy"};
            String text = format("{}, thickness {}", names[s], int(thicknesses[t]));
            caption(PrRect{d.origin.x, y + 12, d.origin.x + d.length, y + 32}, text);
        }
        y += 110;
    }
    // Wavy fragments whose phases continue across them, like a line of text split into runs.
    float x = 60;
    PrColor tints[] = {kRed, kBlue, kGreen};
    for (uint32_t i = 0; i < 3; ++i) {
        PrDrawDecoration d{};
        d.origin = PrVec2{x, y};
        d.length = 150 + float(i) * 37;
        d.thickness = 2;
        d.style = PrDrawDecorationStyle_Wavy;
        d.wavelength = 20;
        d.amplitude = 4;
        d.phase = x - 60;
        prDrawListDecoration(list, &d, {.color = tints[i]});
        x += d.length;
    }
    caption(PrRect{60, y + 12, x, y + 32}, "wavy fragments whose phase continues across them");
    (void)height;
}

} // namespace

const Array<Drawing>& boxFeatures() {
    static const Array<Drawing> features({
        {"Border styles", "Each border style at width 18, square and round corners alternating.", borderStyles},
        {
            "Border sides and corners",
            "Sides of different widths, colors and styles, shaped corners, patterns and brushes.",
            borderSides,
        },
        {"Shadows", "Drop and inner shadows: offsets, blurs, spreads and scaling.", shadows},
        {"Decorations", "Each decoration style at three thicknesses, and wavy fragments.", decorations},
    });
    return features;
}

} // namespace drawings
