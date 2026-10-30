// Feature drawings for text: glyph runs shaped by lib/text from fonts that lib/font reads.

#include "tools/viewer/features_text.h"

#include "pr/vec.h"

#include "tools/viewer/drawing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace viewer {

namespace {

constexpr uint32_t kWght = PR_FONT_TAG('w', 'g', 'h', 't');
constexpr uint32_t kWdth = PR_FONT_TAG('w', 'd', 't', 'h');

TextStyle styleOf(const char* file, float size, PrColor tint = kInk) {
    TextStyle style;
    style.font = fontNamed(file);
    style.size = size;
    style.paint.color = tint;
    return style;
}

// Draws a hairline across the drawing at `y`, from `left` to `right`.
void guide(PrDrawList* list, float left, float right, float y, PrColor tint) {
    PrDrawStroke hairline = hairlineStroke();
    prDrawListLine(list, PrVec2{left, y}, PrVec2{right, y}, {.color = tint, .stroke = &hairline});
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawings

void textBasics(PrDrawList* list, float width, float, double) {
    const char* pangram = "The quick brown fox jumps over the lazy dog. 0123456789";
    float y = 40;
    for (float size : {9.0f, 12.0f, 16.0f, 24.0f, 36.0f}) {
        char text[16];
        std::snprintf(text, sizeof(text), "%g", double(size));
        label(text, PrVec2{20, y});
        drawText(list, styleOf("NotoSans.ttf", size), pangram, PrVec2{60, y});
        y += size * 1.5f + 8;
    }
    drawText(list, styleOf("NotoSerif.ttf", 30), "Noto Serif: Handgloves & Quizzical Jabs", PrVec2{60, y + 10});

    // A line's metrics, from the font, around large text.
    const PrFont* font = fontNamed("NotoSans.ttf");
    PrFontMetrics m = prFontMetrics(font);
    float size = 110;
    PrVec2 origin{60, 520};
    float right = width - 40;
    float advance = drawText(list, styleOf("NotoSans.ttf", size, kInk), "Hxgy \xc3\x80\xc3\xa9", origin);
    struct Line {
        const char* name;
        float y;
        PrColor tint;
    };
    const Line lines[] = {
        {"ascent", -m.ascent, kRed},     {"cap height", -m.capHeight, kGold},
        {"x-height", -m.xHeight, kGreen}, {"baseline", 0, kInk},
        {"descent", m.descent, kBlue},
    };
    for (const Line& line : lines) {
        float at = origin.y + line.y * size;
        guide(list, origin.x - 10, right, at, withAlpha(line.tint, 0.8f));
        label(line.name, PrVec2{origin.x + advance + 20, at - 3}, 11, line.tint);
    }
    // Underline and strikeout where the font puts them.
    PrDrawDecoration underline{};
    underline.origin = PrVec2{origin.x, origin.y + m.underlinePosition * size};
    underline.length = advance;
    underline.thickness = m.underlineThickness * size;
    prDrawListDecoration(list, &underline, {.color = withAlpha(kPurple, 0.7f)});
    PrDrawDecoration strike = underline;
    strike.origin.y = origin.y + m.strikeoutPosition * size;
    strike.thickness = m.strikeoutThickness * size;
    prDrawListDecoration(list, &strike, {.color = withAlpha(kTeal, 0.7f)});

    // Translucent tints overlapping, each glyph compositing on its own.
    PrVec2 at{60, 720};
    drawText(list, styleOf("NotoSans.ttf", 64, withAlpha(kRed, 0.6f)), "Overlap", at);
    drawText(list, styleOf("NotoSans.ttf", 64, withAlpha(kBlue, 0.6f)), "Overlap", PrVec2{at.x + 24, at.y + 14});
    TextStyle tight = styleOf("NotoSans.ttf", 64, withAlpha(kGreen, 0.5f));
    PrTextFeature spacing{PR_FONT_TAG('k', 'e', 'r', 'n'), 1, 0, 0xffffffffu};
    tight.features = &spacing;
    tight.featureCount = 1;
    drawText(list, tight, "AVAWAY", PrVec2{420, 720});
    label("translucent runs overlapping, each glyph on its own", PrVec2{60, 640});
    label("kerned, half alpha", PrVec2{420, 640});
}

// Draws runs that exercise the shaper: ligatures, kerning, features, marks and complex scripts.
void shaping(PrDrawList* list, float, float, double) {
    struct Row {
        const char* name;
        const char* font;
        const char* text;
        PrTextDirection direction;
        const char* language;
        PrTextFeature feature;
    };
    const PrTextFeature none{0, 0, 0, 0};
    const Row rows[] = {
        {"ligatures", "NotoSerif.ttf", "office affine flow", PrTextDirection_Ltr, nullptr, none},
        {
            "ligatures off", "NotoSerif.ttf", "office affine flow", PrTextDirection_Ltr,
            nullptr, {PR_FONT_TAG('l', 'i', 'g', 'a'), 0, 0, 0xffffffffu},
        },
        {"kerning", "NotoSans.ttf", "AVATAR Toyota WAVE", PrTextDirection_Ltr, nullptr, none},
        {
            "small caps", "NotoSans.ttf", "Small Capitals", PrTextDirection_Ltr, nullptr,
            {PR_FONT_TAG('s', 'm', 'c', 'p'), 1, 0, 0xffffffffu},
        },
        {
            "fractions", "NotoSans.ttf", "1/2 3/4 5/8", PrTextDirection_Ltr, nullptr,
            {PR_FONT_TAG('f', 'r', 'a', 'c'), 1, 0, 0xffffffffu},
        },
        {
            "marks", "NotoSans.ttf", "e\xcc\x81 n\xcc\x83 q\xcc\x87\xcc\xa3 a\xcc\x8a\xcc\x81",
            PrTextDirection_Ltr, nullptr, none,
        },
        {
            "Arabic", "NotoSansArabic.ttf",
            "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84" "\xd9\x85",
            PrTextDirection_Rtl, "ar", none,
        },
        {
            "Arabic marks", "NotoSansArabic.ttf",
            "\xd8\xa8\xd9\x90\xd8\xb3\xd9\x92\xd9\x85\xd9\x90 \xd8\xa7\xd9\x84\xd9\x84\xd9\x91\xd9\x8e"
                "\xd9\x87\xd9\x90",
            PrTextDirection_Rtl, "ar", none,
        },
        {
            "Devanagari", "NotoSansDevanagari.ttf",
            "\xe0\xa4\xa8\xe0\xa4\xae\xe0\xa4\xb8\xe0\xa5\x8d\xe0\xa4\xa4\xe0\xa5\x87 \xe0\xa4\x95\xe0"
                "\xa5\x8d\xe0\xa4\xb7\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa4\xbf\xe0\xa4\xaf",
            PrTextDirection_Ltr, "hi", none,
        },
        {
            "Devanagari reph", "NotoSansDevanagari.ttf",
            "\xe0\xa4\xb0\xe0\xa5\x8d\xe0\xa4\x95 \xe0\xa4\xa7\xe0\xa4\xb0\xe0\xa5\x8d\xe0\xa4\xae "
                "\xe0\xa4\xb6\xe0\xa5\x8d\xe0\xa4\xb0\xe0\xa5\x80",
            PrTextDirection_Ltr, "hi", none,
        },
    };
    float y = 80;
    for (const Row& row : rows) {
        label(row.name, PrVec2{20, y});
        TextStyle style = styleOf(row.font, 26);
        style.direction = row.direction;
        style.language = row.language;
        if (row.feature.tag != 0) {
            style.features = &row.feature;
            style.featureCount = 1;
        }
        drawText(list, style, row.text, PrVec2{150, y});
        y += 70;
    }
}

void variations(PrDrawList* list, float width, float, double) {
    // Weight and width over the variable Noto Sans.
    float y = 50;
    float x = 30;
    for (float weight = 100; weight <= 900; weight += 100) {
        PrFontVariation v{kWght, weight};
        TextStyle style = styleOf("NotoSans.ttf", 30);
        style.font = fontVaried("NotoSans.ttf", &v, 1);
        char text[32];
        std::snprintf(text, sizeof(text), "Weight %g", double(weight));
        float w = drawText(list, style, text, PrVec2{x, y});
        x += w + 24;
        if (x > width - 220) {
            x = 30;
            y += 48;
        }
    }
    y += 70;
    for (float stretch : {62.5f, 75.0f, 87.5f, 100.0f}) {
        PrFontVariation v[2] = {{kWdth, stretch}, {kWght, 600}};
        TextStyle style = styleOf("NotoSans.ttf", 34);
        style.font = fontVaried("NotoSans.ttf", v, 2);
        char text[48];
        std::snprintf(text, sizeof(text), "Width %g: condensed to normal", double(stretch));
        drawText(list, style, text, PrVec2{30, y});
        y += 46;
    }
    // Nabla: a color font whose layers vary with its axes, edge depth and highlight.
    y += 60;
    constexpr uint32_t kEdpt = PR_FONT_TAG('E', 'D', 'P', 'T');
    constexpr uint32_t kEhlt = PR_FONT_TAG('E', 'H', 'L', 'T');
    const float depths[] = {0, 100, 200};
    const float highlights[] = {0, 12, 24};
    for (uint32_t i = 0; i < 3; ++i) {
        PrFontVariation v[2] = {{kEdpt, depths[i]}, {kEhlt, highlights[i]}};
        TextStyle style = styleOf("Nabla.ttf", 88);
        style.font = fontVaried("Nabla.ttf", v, 2);
        char text[48];
        std::snprintf(text, sizeof(text), "EDPT %g EHLT %g", double(depths[i]), double(highlights[i]));
        label(text, PrVec2{30 + 350 * float(i), y - 10});
        drawText(list, style, "Nabla", PrVec2{30 + 350 * float(i), y + 90});
    }
}

void syntheticAndTransforms(PrDrawList* list, float width, float height, double) {
    // Synthetic bold and oblique, against the real weight.
    float y = 60;
    struct Style {
        const char* name;
        float embolden;
        float oblique;
    };
    const Style styles[] = {
        {"regular", 0, 0},     {"embolden 0.02", 0.02f, 0},
        {"embolden 0.04", 0.04f, 0}, {"embolden -0.01", -0.01f, 0},
        {"oblique 0.2", 0, 0.2f},    {"both", 0.03f, 0.2f},
    };
    for (uint32_t i = 0; i < 6; ++i) {
        TextStyle style = styleOf("NotoSans.ttf", 32);
        PrFontDerivation d = prFontDerivationDefault();
        d.embolden = styles[i].embolden;
        d.oblique = styles[i].oblique;
        style.font = fontDerived("NotoSans.ttf", d);
        float x = 30 + float(i % 3) * 360;
        float at = y + float(i / 3) * 60;
        label(styles[i].name, PrVec2{x, at - 36});
        drawText(list, style, "Synthetic", PrVec2{x, at});
    }
    PrFontVariation bold{kWght, 700};
    TextStyle real = styleOf("NotoSans.ttf", 32);
    real.font = fontVaried("NotoSans.ttf", &bold, 1);
    label("weight 700", PrVec2{30, y + 120 - 36});
    drawText(list, real, "Synthetic", PrVec2{30, y + 120});

    // Text on a circle: each glyph turned about its origin to the tangent.
    TextStyle circle = styleOf("NotoSerif.ttf", 22, kPurple);
    ShapedText shaped;
    const char* text = "Per-glyph transforms set text along a circle \xe2\x80\xa2 ";
    float length = shapeText(circle, text, PrVec2{0, 0}, shaped);
    PrVec2 center{230, 470};
    float radius = 150;
    float start = -kPi / 2 - length / radius / 2;
    shaped.transforms.resize(shaped.glyphs.size(), prAffine2Identity());
    for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
        PrFontGlyph& g = shaped.glyphs[i];
        float angle = start + g.position.x / radius;
        g.position = PrVec2{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
        shaped.transforms[i] = prAffine2Concat(shaped.transforms[i], prAffine2Rotate(angle + kPi / 2));
    }
    drawGlyphs(list, circle, shaped);

    // A wave: each glyph raised and turned with the slope.
    TextStyle wave = styleOf("NotoSans.ttf", 30, kTeal);
    shapeText(wave, "Glyphs riding a sine wave", PrVec2{430, 420}, shaped);
    shaped.transforms.resize(shaped.glyphs.size(), prAffine2Identity());
    for (size_t i = 0; i < shaped.glyphs.size(); ++i) {
        PrFontGlyph& g = shaped.glyphs[i];
        float phase = (g.position.x - 430) / 60;
        g.position.y += 24 * std::sin(phase);
        shaped.transforms[i] = prAffine2Concat(shaped.transforms[i],
            prAffine2Rotate(std::atan(24.0f / 60 * std::cos(phase))));
    }
    drawGlyphs(list, wave, shaped);

    // The run's own transform: rotated, vertical, and scaled from tiny to huge.
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(-kPi / 12), prAffine2Translate(PrVec2{450, 560})));
    drawText(list, styleOf("NotoSans.ttf", 28, kRed), "A rotated run", PrVec2{0, 0});
    prDrawListPopTransform(list);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(kPi / 2), prAffine2Translate(PrVec2{width - 60, 380})));
    drawText(list, styleOf("NotoSans.ttf", 24, kBlue), "Turned a quarter", PrVec2{0, 0});
    prDrawListPopTransform(list);
    float x = 450;
    for (float size : {5.0f, 7.0f, 9.0f, 11.0f}) {
        drawText(list, styleOf("NotoSans.ttf", size), "tiny text", PrVec2{x, 640});
        x += size * 6;
    }
    drawText(list, styleOf("NotoSerif.ttf", 260, withAlpha(kGold, 0.8f)), "g", PrVec2{width - 300, height - 70});
    label("sizes 5, 7, 9 and 11", PrVec2{450, 615});
    label("one glyph at 260, translucent", PrVec2{width - 300, height - 250});
    label("per-glyph transforms", PrVec2{110, 480});
}

void strokedAndPainted(PrDrawList* list, float width, float, double) {
    PrRect grid{20, 20, width - 20, 780};
    float columnWidth = prRectWidth(grid) / 2;
    auto cellOrigin = [&](uint32_t i) {
        return PrVec2{grid.left + float(i % 2) * columnWidth + 10, 120 + float(i / 2) * 160};
    };
    // Strokes of the outlines: joins, dashes, painted over a fill, a hairline.
    PrDrawStroke round = strokeOf(3, PrPathJoin_Round, PrPathCap_Round);
    TextStyle style = styleOf("NotoSans.ttf", 96, kBlue);
    style.paint.stroke = &round;
    label("stroked, round joins", PrVec2{cellOrigin(0).x, cellOrigin(0).y - 90});
    drawText(list, style, "Stroke", cellOrigin(0));

    PrDrawStroke dashed = strokeOf(2.5f, PrPathJoin_Miter, PrPathCap_Butt);
    const float dashes[] = {8, 5};
    dashed.shape.dashes = dashes;
    dashed.shape.dashCount = 2;
    style.paint.stroke = &dashed;
    style.paint.color = kRed;
    label("dashed", PrVec2{cellOrigin(1).x, cellOrigin(1).y - 90});
    drawText(list, style, "Dashes", cellOrigin(1));

    PrDrawStroke outline = strokeOf(10, PrPathJoin_Round, PrPathCap_Round);
    TextStyle under = styleOf("NotoSans.ttf", 96, kGold);
    under.paint.stroke = &outline;
    label("a wide stroke under the fill", PrVec2{cellOrigin(2).x, cellOrigin(2).y - 90});
    drawText(list, under, "Layered", cellOrigin(2));
    drawText(list, styleOf("NotoSans.ttf", 96, kInk), "Layered", cellOrigin(2));

    PrDrawStroke outside = strokeOf(6, PrPathJoin_Round, PrPathCap_Round);
    outside.align = PrDrawStrokeAlign_Outside;
    TextStyle aligned = styleOf("NotoSans.ttf", 96, kTeal);
    aligned.paint.stroke = &outside;
    label("aligned outside, and a hairline", PrVec2{cellOrigin(3).x, cellOrigin(3).y - 90});
    drawText(list, aligned, "Outside", cellOrigin(3));
    PrDrawStroke hairline = hairlineStroke();
    TextStyle thin = styleOf("NotoSans.ttf", 96, kInk);
    thin.paint.stroke = &hairline;
    drawText(list, thin, "Outside", cellOrigin(3));

    // Brushes over the run: a gradient in its bounds, an image, a stroke's brush along it.
    const PrDrawColorStop sunset[] = {
        {.offset = 0, .color = kPurple},
        {.offset = 0.5f, .color = kRed},
        {.offset = 1, .color = kGold},
    };
    PrDrawBrush bounds = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{1, 1}, sunset, 3);
    bounds.space = PrDrawBrushSpace_ObjectBounds;
    label("a gradient over the run's bounds", PrVec2{cellOrigin(4).x, cellOrigin(4).y - 90});
    TextStyle heavy = styleOf("NotoSans.ttf", 96, kWhite);
    PrFontVariation black{kWght, 900};
    heavy.font = fontVaried("NotoSans.ttf", &black, 1);
    TextStyle brushed = heavy;
    brushed.paint.brush = &bounds;
    drawText(list, brushed, "Sunset", cellOrigin(4));

    PrDrawBrush tiles = prDrawBrushDefault(PrDrawBrushKind_Image);
    tiles.image.source = prDrawSourceImage(testCard(), PrRect{0, 0, 64, 64});
    tiles.image.tiles.tile = PrRect{0, 0, 32, 32};
    tiles.image.tiles.x = PrDrawExtend_Repeat;
    tiles.image.tiles.y = PrDrawExtend_Repeat;
    tiles.image.sampling = PrDrawSampling_Linear;
    label("an image brush", PrVec2{cellOrigin(5).x, cellOrigin(5).y - 90});
    brushed.paint.brush = &tiles;
    drawText(list, brushed, "Pattern", cellOrigin(5));

    const PrDrawColorStop rainbow[] = {
        {.offset = 0, .color = kRed},
        {.offset = 0.25f, .color = kGold},
        {.offset = 0.5f, .color = kGreen},
        {.offset = 0.75f, .color = kBlue},
        {.offset = 1, .color = kPurple},
    };
    PrDrawBrush along = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{1, 0}, rainbow, 5);
    label("a gradient along each contour", PrVec2{cellOrigin(6).x, cellOrigin(6).y - 90});
    PrDrawStroke mapped = strokeOf(5, PrPathJoin_Round, PrPathCap_Round);
    mapped.mapping = PrDrawStrokeMapping_Along;
    TextStyle traced = styleOf("NotoSerif.ttf", 96, kWhite);
    traced.paint.brush = &along;
    traced.paint.stroke = &mapped;
    drawText(list, traced, "Along", cellOrigin(6));

    label("tinted half alpha over stripes", PrVec2{cellOrigin(7).x, cellOrigin(7).y - 90});
    PrVec2 o = cellOrigin(7);
    for (float x = o.x; x < o.x + 420; x += 24)
        prDrawListRect(list, PrRect{x, o.y - 80, x + 12, o.y + 20}, {.color = withAlpha(kGreen, 0.5f)});
    drawText(list, styleOf("NotoSans.ttf", 96, withAlpha(kInk, 0.5f)), "Veiled", o);
}

void colorGlyphs(PrDrawList* list, float, float, double) {
    // Emoji in Twemoji's COLRv1 layers, including sequences that the shaper joins.
    label("Twemoji (COLRv1): single emoji, a skin tone, ZWJ sequences, flags", PrVec2{30, 30});
    TextStyle emoji = styleOf("Twemoji.ttf", 48);
    drawText(list, emoji, "\xf0\x9f\x98\x80\xf0\x9f\x98\x82\xf0\x9f\xa5\xb0\xf0\x9f\xa4\x96\xf0\x9f\x91\xbd\xf0"
        "\x9f\x8e\x83\xf0\x9f\x8c\x8d\xf0\x9f\x8d\x89\xf0\x9f\x9a\x80\xe2\x9a\xbd\xf0\x9f\x8e" "\xb8\xf0\x9f\x92\x8e",
        PrVec2{30, 95});
    drawText(list, emoji, "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x9a\x80 "
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 "
        "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5\xf0\x9f\x87\xab\xf0\x9f\x87\xb7\xf0\x9f\x87\xa7\xf0"
        "\x9f\x87\xb7", PrVec2{30, 160});
    // Small and large, translucent, and stroked, which outlines the glyph rather than its layers.
    TextStyle small = styleOf("Twemoji.ttf", 16);
    drawText(list, small, "\xf0\x9f\x98\x80\xf0\x9f\x9a\x80\xf0\x9f\x8c\x88", PrVec2{760, 80});
    TextStyle big = styleOf("Twemoji.ttf", 110, withAlpha(kWhite, 0.5f));
    drawText(list, big, "\xf0\x9f\x8c\x88", PrVec2{860, 170});
    PrDrawStroke outline = strokeOf(1.5f);
    TextStyle stroked = styleOf("Twemoji.ttf", 48, kInk);
    stroked.paint.stroke = &outline;
    drawText(list, stroked, "\xf0\x9f\x98\x80", PrVec2{760, 160});

    // Nabla's palettes.
    const PrFont* nabla = fontNamed("Nabla.ttf");
    uint32_t palettes = prFontPaletteCount(nabla);
    float x = 30;
    for (uint32_t p = 0; p < palettes && p < 7; ++p) {
        TextStyle style = styleOf("Nabla.ttf", 64);
        style.palette = p;
        char text[24];
        std::snprintf(text, sizeof(text), "palette %u", p);
        label(text, PrVec2{x, 215});
        x += drawText(list, style, "Ab", PrVec2{x, 290}) + 20;
    }

    // Lettering whose layers fill with gradients, in a palette other than the first.
    label("Bungee Spice and Kalnia Glaze (COLRv1): gradient lettering", PrVec2{30, 330});
    drawText(list, styleOf("BungeeSpice.ttf", 64), "Color Type", PrVec2{30, 410});
    TextStyle glaze = styleOf("KalniaGlaze.ttf", 72);
    PrFontVariation bold{kWght, 700};
    glaze.font = fontVaried("KalniaGlaze.ttf", &bold, 1);
    glaze.palette = 1;
    drawText(list, glaze, "Glazed Letters", PrVec2{30, 510});
}

void textEffects(PrDrawList* list, float width, float height, double) {
    PrFontVariation black{kWght, 850};
    TextStyle heavy = styleOf("NotoSans.ttf", 80);
    heavy.font = fontVaried("NotoSans.ttf", &black, 1);

    // A drop shadow from a filter on the group the text draws in.
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode drop{};
    drop.kind = PrDrawFilterKind_DropShadow;
    drop.space = PrColorSpace_Srgb;
    drop.inputs[0] = PrDrawFilterInput_Source;
    drop.inputs[1] = PrDrawFilterInput_Source;
    drop.shadow.offset = PrVec2{5, 6};
    drop.shadow.sigma = PrVec2{4, 4};
    drop.shadow.color = withAlpha(kInk, 0.6f);
    prDrawFilterBuilderAdd(builder, &drop);
    const PrDrawFilter* shadow = nullptr;
    shadow = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawGroup group = prDrawGroupDefault();
    group.filter = shadow;
    group.bounds = PrRect{20, 20, 540, 160};
    prDrawListPushGroup(list, &group);
    heavy.paint.color = kGold;
    drawText(list, heavy, "Shadowed", PrVec2{30, 120});
    prDrawListPopGroup(list);
    prDrawFilterRelease(shadow);

    // Blend modes over a gradient.
    PrRect band{580, 30, width - 30, 150};
    const PrDrawColorStop stops[] = {{.offset = 0, .color = kBlue}, {.offset = 1, .color = kGold}};
    PrDrawBrush gradient = prDrawBrushLinear(PrVec2{band.left, 0}, PrVec2{band.right, 0}, stops, 2);
    prDrawListRect(list, band, {.brush = &gradient});
    const PrDrawBlendMode modes[] = {PrDrawBlendMode_Multiply, PrDrawBlendMode_Screen, PrDrawBlendMode_Difference};
    const char* names[] = {"Mul", "Scr", "Dif"};
    for (uint32_t i = 0; i < 3; ++i) {
        TextStyle style = heavy;
        style.size = 64;
        style.paint.color = kRed;
        style.paint.blendMode = modes[i];
        drawText(list, style, names[i], PrVec2{band.left + 10 + 165 * float(i), band.bottom - 35});
    }

    // Knockout: the text erases an isolated group's contents.
    PrRect card{30, 200, 540, 360};
    PrDrawGroup isolated = prDrawGroupDefault();
    isolated.isolated = true;
    prDrawListPushGroup(list, &isolated);
    PrCornerRect rounded = prCornerRectRound(card, 16);
    prDrawListCornerRect(list, rounded, {.color = kPurple});
    TextStyle erasing = heavy;
    erasing.paint.color = kInk;
    erasing.paint.composite = PrDrawComposite_DestinationOut;
    drawText(list, erasing, "Knockout", PrVec2{card.left + 40, card.bottom - 50});
    prDrawListPopGroup(list);

    // Clipped to text: an image shown through a mask the text draws.
    PrRect photo{580, 200, width - 30, 360};
    PrDrawList* maskList = prDrawListCreate();
    TextStyle maskText = heavy;
    maskText.size = 110;
    maskText.paint.color = kWhite;
    drawText(maskList, maskText, "IMAGE", PrVec2{photo.left + 10, photo.bottom - 30});
    PrDrawList* textMask = maskList;
    PrDrawSource source = prDrawSourceList(textMask, photo);
    PrDrawBrush brush = prDrawBrushImage(source, photo, PrDrawSampling_Linear);
    PrDrawMask mask = prDrawMaskDefault(&brush);
    PrDrawGroup masked = prDrawGroupDefault();
    masked.mask = &mask;
    masked.bounds = photo;
    prDrawListPushGroup(list, &masked);
    drawImage(list, testCard(), PrRect{0, 0, 64, 64}, photo, PrDrawSampling_Cubic, 1);
    prDrawListPopGroup(list);
    prDrawListRelease(textMask);

    // Decorations from the font's metrics, of every style.
    const PrFont* font = fontNamed("NotoSans.ttf");
    PrFontMetrics m = prFontMetrics(font);
    const PrDrawDecorationStyle styles[] = {
        PrDrawDecorationStyle_Solid,
        PrDrawDecorationStyle_Double,
        PrDrawDecorationStyle_Dotted,
        PrDrawDecorationStyle_Dashed,
        PrDrawDecorationStyle_Wavy,
    };
    const char* styleNames[] = {
        "Solid underline", "Double underline", "Dotted underline",
        "Dashed underline", "Wavy underline",
    };
    for (uint32_t i = 0; i < 5; ++i) {
        float size = 30;
        PrVec2 at{30 + float(i % 3) * 360, 440 + float(i / 3) * 70};
        float advance = drawText(list, styleOf("NotoSans.ttf", size), styleNames[i], at);
        PrDrawDecoration d{};
        d.origin = PrVec2{at.x, at.y + m.underlinePosition * size * 1.4f};
        d.length = advance;
        d.thickness = std::max(m.underlineThickness * size, 1.0f);
        d.style = styles[i];
        d.wavelength = 12;
        d.amplitude = 2;
        prDrawListDecoration(list, &d, {.color = kRed});
    }

    // Lines of text receding in perspective, tilted back about their last baseline.
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, 0.95f);
    PrMatrix4 eye = prMatrix4Perspective(420);
    PrMatrix4 place = prMatrix4Translate(PrVec3{width / 2, height - 30, 0});
    PrMatrix4 m1 = prMatrix4Concat(&tilt, &eye);
    PrMatrix4 m2 = prMatrix4Concat(&m1, &place);
    const char* crawl[] = {"A long time ago, in a viewer", "far, far away, glyphs were", "drawn in perspective."};
    TextStyle line = styleOf("NotoSans.ttf", 60, kGold);
    float widest = 0;
    for (const char* text : crawl)
        widest = std::max(widest, textAdvance(line, text));
    PrDrawGroup plane = prDrawGroupDefault();
    plane.bounds = PrRect{-widest / 2 - 8, -230, widest / 2 + 8, 30};
    plane.projection = &m2;
    prDrawListPushGroup(list, &plane);
    for (uint32_t i = 0; i < 3; ++i) {
        float advance = textAdvance(line, crawl[i]);
        drawText(list, line, crawl[i], PrVec2{-advance / 2, -150 + 75 * float(i)});
    }
    prDrawListPopGroup(list);
    label("a drop shadow filter on the group the text draws in", PrVec2{30, 40});
    label("multiply, screen and difference over a gradient", PrVec2{580, 24});
    label("knockout: destination-out in an isolated group", PrVec2{30, 380});
    label("an image masked by the text", PrVec2{580, 380});
    label("decorations of each style, from the font's metrics", PrVec2{30, 555});
    label("lines receding in perspective", PrVec2{30, 615});
}

} // namespace

const std::vector<Drawing>& textFeatures() {
    static const std::vector<Drawing> features = {
        {"Text", "Sizes and fonts, a font's metrics, and translucent runs.", textBasics},
        {"Shaping", "Ligatures, kerning, features, marks and complex scripts, shaped.", shaping},
        {"Variable fonts", "Design points of variable fonts, color layers included.", variations},
        {
            "Synthetic styles and glyph transforms",
            "Synthetic bold and oblique, per-glyph transforms, and runs under transforms.",
            syntheticAndTransforms,
        },
        {"Stroked and painted text", "Glyph outlines stroked, and runs painted by brushes.", strokedAndPainted},
        {"Color glyphs", "Glyphs drawn in color from their COLR layers.", colorGlyphs},
        {"Text effects", "Text in groups, blends, masks and perspective, with decorations.", textEffects},
    };
    return features;
}

} // namespace viewer
