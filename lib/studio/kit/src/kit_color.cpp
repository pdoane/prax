// Studio Kit's colors: color fields, and the picker that edits a color in HSV, in OKLCH, or from a tonal ramp.

#include "pr/kit_color.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/kit_choice.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// The keys of the retained state of this file's widgets.
enum : uint64_t {
    ColorStateKey_Picker = KitStateKey_Color,
};

// The side of a picker's plane, the width of a strip beside it and the gap between them, the side of the plane's marker
// and the height of a strip's, in px. The picker is as wide as the plane and its two strips.
constexpr float kPlaneSize = 200;
constexpr float kStripWidth = 16;
constexpr float kStripGap = 8;              // the fourth step of the spacing scale
constexpr float kPickerWidth = kPlaneSize + 2 * (kStripGap + kStripWidth);
constexpr float kPlaneMarker = 10;
constexpr float kStripMarker = 4;

// The side of a square of the checkerboard that shows through a color's transparency, in px.
constexpr float kChecker = 4;

// The pixels across and down that a perceptual plane is sampled at, which the plane stretches over its box.
constexpr uint32_t kSamples = 100;

// The most chroma that the perceptual planes reach, which is past the display's gamut so that its edge shows.
constexpr float kMaxChroma = 0.4f;

// The lightness and the chroma that the perceptual strip of hues is drawn at, where its hues read most evenly.
constexpr float kHueLightness = 0.65f;
constexpr float kHueChroma = 0.13f;

// The lightness at which a tonal ramp's chroma peaks, where its plane is sampled and a hue's reach is measured, and the
// steps of the search for that reach.
constexpr float kPeakLightness = 0.64f;
constexpr uint32_t kPeakSteps = 18;

// The tones of a ramp, lightest first: their lightness, and their chroma as a fraction of the ramp's. The lightness
// steps evenly and the chroma curves up through the middle tones, so that a tone reads as a shade of one color rather
// than as a fade.
constexpr uint32_t kRampTones = 11;
constexpr float kRampLightness[kRampTones] = {
    0.97f, 0.93f, 0.87f, 0.80f, 0.71f, 0.64f,
    0.58f, 0.51f, 0.45f, 0.39f, 0.27f,
};
constexpr float kRampChroma[kRampTones] = {0.10f, 0.22f, 0.42f, 0.65f, 0.90f, 1.00f, 0.97f, 0.85f, 0.70f, 0.55f, 0.35f};

// ---------------------------------------------------------------------------------------------------------------------
// Color spaces

float clampUnit(float value) {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

float clampRange(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

bool sameColor(PrColor a, PrColor b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

uint8_t channelByte(float value) {
    return uint8_t(clampUnit(value) * 255 + 0.5f);
}

// A color's coordinates in OKLCH: lightness from 0 to 1, chroma, and hue in degrees.
struct Oklch {
    float lightness;
    float chroma;
    float hue;
};

Oklch oklchOf(PrColor color) {
    double in[3] = {double(color.r), double(color.g), double(color.b)};
    double out[3] = {};
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_Oklch, in, out);
    return Oklch{float(out[0]), float(out[1]), float(out[2])};
}

// Returns the sRGB channels of `c`, which may be outside [0, 1].
void srgbOf(Oklch c, double out[3]) {
    double in[3] = {double(c.lightness), double(c.chroma), double(c.hue)};
    prColorConvert(PrColorSpace_Oklch, PrColorSpace_Srgb, in, out);
}

// Returns true if the RGB space `gamut` holds `c`.
bool inGamut(Oklch c, PrColorSpace gamut) {
    constexpr double kSlack = 1e-4;
    double in[3] = {double(c.lightness), double(c.chroma), double(c.hue)};
    double rgb[3];
    prColorConvert(PrColorSpace_Oklch, gamut, in, rgb);
    for (uint32_t i = 0; i < 3; ++i) {
        if (rgb[i] < -kSlack || rgb[i] > 1 + kSlack)
            return false;
    }
    return true;
}

// Returns the extended sRGB color of `c` with `alpha`, mapped into `gamut` if `gamut` does not hold it
// (prColorGamutMap).
PrColor shown(Oklch c, float alpha, PrColorSpace gamut) {
    double rgb[3];
    srgbOf(c, rgb);
    PrColor color{float(rgb[0]), float(rgb[1]), float(rgb[2]), alpha};
    return inGamut(c, gamut) ? color : prColorGamutMap(color, gamut, 1);
}

// Returns the most chroma that `gamut` holds at the ramp's peak lightness and `hue`.
float peakChroma(float hue, PrColorSpace gamut) {
    float low = 0;
    float high = kMaxChroma;
    for (uint32_t i = 0; i < kPeakSteps; ++i) {
        float middle = (low + high) / 2;
        if (inGamut(Oklch{kPeakLightness, middle, hue}, gamut))
            low = middle;
        else
            high = middle;
    }
    return low;
}

// Returns tone `index` of the ramp at `hue` whose chroma peaks at `peak`, in `gamut`.
PrColor rampTone(float hue, float peak, uint32_t index, PrColorSpace gamut) {
    return shown(Oklch{kRampLightness[index], peak * kRampChroma[index], hue}, 1, gamut);
}

// Returns the sRGB color of a hue in degrees, a saturation and a value in percent, and an alpha.
PrColor srgbOfHsv(float hue, float saturation, float value, float alpha) {
    float h = hue - prF32Floor(hue / 360) * 360;
    float s = clampRange(saturation, 0, 100) / 100;
    float v = clampRange(value, 0, 100) / 100;
    float sixths = h / 60;
    float sector = prF32Floor(sixths);
    float f = sixths - sector;
    float p = v * (1 - s);
    float q = v * (1 - s * f);
    float u = v * (1 - s * (1 - f));
    switch (int32_t(sector)) {
        case 0:
            return PrColor{v, u, p, alpha};
        case 1:
            return PrColor{q, v, p, alpha};
        case 2:
            return PrColor{p, v, u, alpha};
        case 3:
            return PrColor{p, q, v, alpha};
        case 4:
            return PrColor{u, p, v, alpha};
        default:
            return PrColor{v, p, q, alpha};
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Picker state

// What a picker keeps between rounds. The coordinates of each mode lead the color while the picker edits it, so that a
// gray keeps its hue; a color that the picker did not write is read back into them. The fields edit copies of the
// coordinates, in their own units, and a copy that a field changed is read back into the coordinates.
struct PickerState {
    PrColorSpace gamut;             // the display's gamut, which the perceptual planes show and pick within

    // HSV: the hue in degrees, and the saturation and the value in percent.
    float hue;
    float saturation;
    float value;

    Oklch oklch;
    float alpha;

    // The ramp: its hue in degrees, and the chroma that it peaks at.
    float rampHue;
    float rampChroma;

    // The fields of the mode's coordinates: three whole numbers, which are the HSV coordinates or, last, the perceptual
    // hue, and the perceptual lightness in percent and chroma. Each also as the last round wrote it.
    int32_t whole[3];
    float lightness;
    float chroma;
    int32_t wholeWritten[3];
    float lightnessWritten;
    float chromaWritten;

    // The channel fields, from 0 to 255, also as the last round wrote them.
    int32_t channels[4];
    int32_t channelsWritten[4];

    PrColor written;                // the color that the picker last read or wrote
    uint64_t drawn[3];              // what the drawings of the plane and the two strips show
    PrLayoutNode hexField;          // the hex field of the last round
    uint32_t tab;                   // the mode that the tabs chose
    uint32_t seededTab;             // the mode that the coordinates were last read for
    char hex[10];                   // the hex field's text
    uint8_t hexLength;
    bool seeded;                 // the coordinates hold `written`
    bool live;                   // a gesture of a drag is open
};

// Reads `color` into the coordinates of every mode, keeping a hue where the color leaves it undefined and a saturation
// at black.
void readColor(PickerState& s, PrColor color) {
    float r = clampUnit(color.r);
    float g = clampUnit(color.g);
    float b = clampUnit(color.b);
    float high = prF32Max(r, prF32Max(g, b));
    float span = high - prF32Min(r, prF32Min(g, b));
    if (span > 0) {
        float sixths = high == r ? (g - b) / span : high == g ? 2 + (b - r) / span : 4 + (r - g) / span;
        s.hue = sixths < 0 ? sixths * 60 + 360 : sixths * 60;
    }
    if (high > 0)
        s.saturation = span / high * 100;
    s.value = high * 100;
    Oklch oklch = oklchOf(color);
    if (oklch.chroma > 1e-4f)
        s.oklch.hue = oklch.hue;
    s.oklch.lightness = oklch.lightness;
    s.oklch.chroma = oklch.chroma;
    s.alpha = color.a;
}

// Reads `color` into the ramp's hue and chroma, or keeps the hue of a gray and takes the most chroma that it reaches.
void readRamp(PickerState& s, PrColor color) {
    Oklch oklch = oklchOf(color);
    if (oklch.chroma > 1e-4f) {
        s.rampHue = oklch.hue;
        s.rampChroma = oklch.chroma;
        return;
    }
    s.rampChroma = peakChroma(s.rampHue, s.gamut);
}

// Writes the fields' copies of the coordinates and the channels.
void writeFields(PickerState& s, PrColor color) {
    if (s.tab == PrKitColorMode_Oklch) {
        s.lightness = s.oklch.lightness * 100;
        s.chroma = s.oklch.chroma;
        s.whole[2] = int32_t(s.oklch.hue + 0.5f);
    } else {
        s.whole[0] = int32_t(s.hue + 0.5f);
        s.whole[1] = int32_t(s.saturation + 0.5f);
        s.whole[2] = int32_t(s.value + 0.5f);
    }
    const float channels[4] = {color.r, color.g, color.b, color.a};
    for (uint32_t i = 0; i < 4; ++i)
        s.channels[i] = channelByte(channels[i]);
    for (uint32_t i = 0; i < 3; ++i)
        s.wholeWritten[i] = s.whole[i];
    s.lightnessWritten = s.lightness;
    s.chromaWritten = s.chroma;
    for (uint32_t i = 0; i < 4; ++i)
        s.channelsWritten[i] = s.channels[i];
}

// Returns the color of the open mode's coordinates.
PrColor composed(const PickerState& s) {
    if (s.tab == PrKitColorMode_Oklch)
        return shown(s.oklch, s.alpha, s.gamut);
    return srgbOfHsv(s.hue, s.saturation, s.value, s.alpha);
}

// ---------------------------------------------------------------------------------------------------------------------
// Records

// The parts that pickers are made of.
enum {
    Part_Picker,
    Part_Row,
    Part_Plane,
    Part_Strip,
    Part_Wide,                      // the ramp's plane, as wide as the picker
    Part_Tones,
};
typedef uint8_t Part;

// The picker: a column of its rows, the picker's width.
struct PickerBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.width = prLayoutLengthPx(kPickerWidth);
    }
};
struct PickerFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
        flex.rowGap = prLayoutLengthPx(kPrKitSpace4);
        flex.shrink = 0;
    }
};

// A row of the picker, `height` px tall if that is nonzero, with its children centered across it and apart by `gap`.
PR_NO_PADDING_BEGIN
struct PickerRowBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float height;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        if (height > 0)
            box.height = prLayoutLengthPx(height);
    }
};
struct PickerRowFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    float gap;

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(gap);
        flex.shrink = 0;
    }
};

// An area that a press and a drag edit, `width` px wide and the plane's height, bordered, which positions its markers.
struct AreaBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Relative;
        box.width = prLayoutLengthPx(width);
        box.height = prLayoutLengthPx(kPlaneSize);
        kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END

// The replaced element that holds an area's drawing, as large as the area.
struct FillBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = prLayoutLengthPercent(100);
        box.height = prLayoutLengthPercent(100);
    }
};

// Returns the record of a part of the picker.
PrLayoutStyleIndex partStyle(PrKit& kit, Part part) {
    switch (part) {
        case Part_Picker:
            return kitStyle(kit, PickerBox{}, PickerFlex{});
        case Part_Row:
            return kitStyle(kit, PickerRowBox{}, PickerRowFlex{.gap = kStripGap});
        case Part_Tones:
            return kitStyle(kit, PickerRowBox{.height = kPrKitHeightMd}, PickerRowFlex{.gap = 2});
        default: {
            float width = part == Part_Plane ? kPlaneSize : part == Part_Strip ? kStripWidth : kPickerWidth;
            return kitStyle(kit, AreaBox{.width = width}, KitFixed{}, KitColors{.border = kit.theme.palette.line2});
        }
    }
}

PrLayoutNode part(PrKit& kit, PrLayoutNode parent, uint64_t key, Part part) {
    return prLayoutTreeFrameElement(kit.tree, parent, key, partStyle(kit, part));
}

// A marker on an area, centered at `x` and `y`, fractions of the area's width and height: a ring for a plane, or a bar
// across a strip if `bar` is set. Its place moves as the color does.
PR_NO_PADDING_BEGIN
struct MarkerBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float x;
    float y;
    bool bar;
    uint8_t _pad[3];

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.inset[PrRectSide_Top] = prLayoutLengthPercent(y * 100);
        if (bar) {
            box.inset[PrRectSide_Left] = prLayoutLengthPx(-2);
            box.inset[PrRectSide_Right] = prLayoutLengthPx(-2);
            box.margin[PrRectSide_Top] = prLayoutLengthPx(-kStripMarker / 2);
            box.height = prLayoutLengthPx(kStripMarker);
        } else {
            box.inset[PrRectSide_Left] = prLayoutLengthPercent(x * 100);
            box.margin[PrRectSide_Left] = prLayoutLengthPx(-kPlaneMarker / 2);
            box.margin[PrRectSide_Top] = prLayoutLengthPx(-kPlaneMarker / 2);
            box.width = prLayoutLengthPx(kPlaneMarker);
            box.height = prLayoutLengthPx(kPlaneMarker);
        }
        kitSetBorderWidth(box, bar ? 1 : 2);
    }
};
PR_NO_PADDING_END

// A marker's decoration: a white border, outlined in the app's background so that it shows over any color.
struct MarkerFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    void make(const PrKit& kit, PrLayoutDecorationStyle& decoration) const {
        kitSetBorderColor(decoration, PrColor{1, 1, 1, 1});
        decoration.outlineColor = kitColor16(kit.theme.palette.bg0);
        decoration.outlineStyle = PrDrawBorderStyle_Solid;
        decoration.outlineWidth = prLayoutUnitFromPx(1);
    }
};

// A tone of the ramp: a share of its row, framed twice as thick while it is the color.
PR_NO_PADDING_BEGIN
struct ToneBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    bool chosen;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPercent(100);
        box.minWidth = prLayoutLengthPx(0);
        kitSetBorderWidth(box, chosen ? 2 : 1);
    }
};

// A color field: a row as tall as its control size, of its swatch and its hex.
struct FieldBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrKitControlSize size;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(kitControlHeight(size));
    }
};

// The focus ring, which a field shows while the keyboard put focus on it.
struct RingFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    bool ring;

    void make(const PrKit& kit, PrLayoutDecorationStyle& decoration) const {
        if (!ring)
            return;
        decoration.outlineColor = kitColor16(kit.theme.palette.accent);
        decoration.outlineStyle = PrDrawBorderStyle_Solid;
        decoration.outlineWidth = prLayoutUnitFromPx(1);
        decoration.outlineOffset = prLayoutUnitFromPx(1);
    }
};
PR_NO_PADDING_END

// ---------------------------------------------------------------------------------------------------------------------
// Drawings

// Sets the drawing of a replaced element `extent` px in size, which `draw` makes, unless `*drawn` is `key` and the
// element holds a drawing already. Writes `key` to `*drawn`.
template <typename Draw>
void setDrawing(PrKit& kit, PrLayoutNode node, PrVec2 extent, uint64_t key, uint64_t* drawn, Draw draw) {
    PrLayoutNaturalSize natural{
        .width = prLayoutUnitFromPx(extent.x),
        .height = prLayoutUnitFromPx(extent.y),
        .ratio = extent.x / extent.y,
    };
    prLayoutTreeFrameSetNaturalSize(kit.tree, node, natural);
    PrLayoutContent held = prLayoutNodeContent(kit.tree, node);
    if (*drawn == key && held.kind == PrLayoutContentKind_DrawList)
        return;
    PrDrawList* list = prDrawListCreate();
    draw(list);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_DrawList;
    content.list = list;
    prLayoutTreeFrameSetContent(kit.tree, node, &content);
    prDrawListRelease(list);
    *drawn = key;
}

// Fills `rect` with a gradient of `stops` along the line from `from` to `to`.
void fillStops(PrDrawList* list, PrRect rect, PrVec2 from, PrVec2 to, const PrDrawColorStop* stops, uint32_t count) {
    PrDrawBrush brush = prDrawBrushLinear(from, to, stops, count);
    prDrawListRect(list, rect, {.brush = &brush});
}

void fillGradient(PrDrawList* list, PrRect rect, PrVec2 from, PrVec2 to, PrColor start, PrColor end) {
    const PrDrawColorStop stops[] = {{.offset = 0, .color = start}, {.offset = 1, .color = end}};
    fillStops(list, rect, from, to, stops, 2);
}

// Fills `rect` with a checkerboard of two grays, under a color's transparency.
void fillChecker(PrDrawList* list, PrRect rect) {
    prDrawListRect(list, rect, {.color = PrColor{0.59f, 0.59f, 0.59f, 1}});
    uint32_t columns = uint32_t(prF32Ceil(prRectWidth(rect) / kChecker));
    uint32_t rows = uint32_t(prF32Ceil(prRectHeight(rect) / kChecker));
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = (y & 1); x < columns; x += 2) {
            PrRect square{
                rect.left + float(x) * kChecker, rect.top + float(y) * kChecker,
                prF32Min(rect.left + float(x + 1) * kChecker, rect.right),
                prF32Min(rect.top + float(y + 1) * kChecker, rect.bottom),
            };
            prDrawListRect(list, square, {.color = PrColor{0.41f, 0.41f, 0.41f, 1}});
        }
    }
}

// The perceptual plane that a sampled drawing shows: lightness across at one hue, or hue across at the ramp's peak
// lightness, chroma up either way.
struct Sampled {
    bool skyline;
    float hue;

    Oklch at(float across, float up) const {
        return skyline ? Oklch{kPeakLightness, up * kMaxChroma, across * 360} : Oklch{across, up * kMaxChroma, hue};
    }
};

// Draws a perceptual plane over `extent`: an image of its samples, each the nearest color that `gamut` holds, and a
// line along the edge of the gamut. If the gamut is wider than sRGB, a dashed line marks where sRGB ends inside it. The
// image holds half floats, so that colors past sRGB reach a target that shows them.
void drawSampled(PrKit& kit, PrDrawList* list, PrVec2 extent, Sampled plane, PrColorSpace gamut) {
    RawArray<PrFloat16> pixels;
    pixels.resize(kSamples * kSamples * 4);
    for (uint32_t y = 0; y < kSamples; ++y) {
        float up = 1 - (float(y) + 0.5f) / kSamples;
        for (uint32_t x = 0; x < kSamples; ++x) {
            PrColor color = shown(plane.at((float(x) + 0.5f) / kSamples, up), 1, gamut);
            PrFloat16* texel = pixels.data + (y * kSamples + x) * 4;
            texel[0] = prFloat16FromFloat(color.r);
            texel[1] = prFloat16FromFloat(color.g);
            texel[2] = prFloat16FromFloat(color.b);
            texel[3] = prFloat16FromFloat(1);
        }
    }
    PrDrawImageDesc desc{
        PrPixelView{reinterpret_cast<const uint8_t*>(pixels.data), kSamples * 8, {kSamples, kSamples}},
        PrPixelFormat_RgbaF16, PrDrawAlpha_Straight, PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    PrRect rect{0, 0, extent.x, extent.y};
    if (prDrawImageCreate(&desc, &image) == PrStatus_Ok) {
        PrDrawBrush brush = prDrawBrushImage(prDrawSourceImage(image, PrRect{0, 0, float(kSamples), float(kSamples)}),
            rect, PrDrawSampling_Linear);
        prDrawListRect(list, rect, {.brush = &brush});
        prDrawImageRelease(image);
    }
    pixels.reset();
    // An edge is where each column, walked up, first leaves a gamut.
    auto edge = [&](PrColorSpace space, const PrDrawStroke& stroke) {
        RawArray<PrVec2> points;
        for (uint32_t x = 0; x < kSamples; ++x) {
            float across = (float(x) + 0.5f) / kSamples;
            for (uint32_t step = 0; step < kSamples; ++step) {
                float up = (float(step) + 0.5f) / kSamples;
                if (inGamut(plane.at(across, up), space))
                    continue;
                points.push(PrVec2{across * extent.x, (1 - up) * extent.y});
                break;
            }
        }
        if (points.count > 1)
            prDrawListPolylineContour(list, points.data, points.count, PrDrawContour_Open, PrPathFillRule_NonZero,
                {.color = kit.theme.palette.text2, .stroke = &stroke});
        points.reset();
    };
    PrDrawStroke solid = prDrawStrokeDefault();
    solid.shape.width = 1;
    edge(gamut, solid);
    if (gamut != PrColorSpace_Srgb) {
        constexpr float kDashes[] = {3, 3};
        PrDrawStroke dashed = solid;
        dashed.shape.dashes = kDashes;
        dashed.shape.dashCount = 2;
        edge(PrColorSpace_Srgb, dashed);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Areas

// Creates a marker centered at `x` and `y`, fractions of `area`'s width and height: a ring for a plane, or a bar across
// a strip if `bar` is true.
void marker(PrKit& kit, PrLayoutNode area, float x, float y, bool bar) {
    prLayoutTreeFrameElement(kit.tree, area, 0, kitStyle(kit, MarkerBox{.x = x, .y = y, .bar = bar}, MarkerFill{}));
}

// An area of the picker that a press and a drag edit, and what the round's drag on it did.
struct Area {
    PrLayoutNode node;
    PrLayoutNode fill;              // the replaced element that holds the area's drawing
    PrVec2 at;                      // the pointer's place, in fractions of the area, if `placed`
    bool placed;
    bool active;                    // the pointer is still down
};

Area area(PrKit& kit, PrLayoutNode parent, Part partId) {
    Area a{};
    a.node = part(kit, parent, 0, partId);
    bool disabled = kitSetFlags(kit, a.node, PrUiFlag_Pressable);
    a.fill = prLayoutTreeFrameReplaced(kit.tree, a.node, 0, kitStyle(kit, FillBox{}));
    if (disabled)
        return a;
    prUiSetCursor(kit.ui, a.node, partId == Part_Strip ? PrAppCursor_Hand : PrAppCursor_Crosshair);
    PrUiDrag drag{};
    if (!prUiDrag(kit.ui, a.node, 0, 0, &drag) || !(drag.active || drag.released))
        return a;
    a.active = drag.active;
    PrVec2 extent = kitExtent(kit, a.node);
    PrVec2 local{};
    if (extent.x <= 0 || extent.y <= 0 || !prUiLocalPoint(kit.ui, a.node, drag.position, &local))
        return a;
    a.at = PrVec2{clampUnit(local.x / extent.x), clampUnit(local.y / extent.y)};
    a.placed = true;
    return a;
}

// Creates the plane of the open mode and the strips beside it, applies their drags to the coordinates, and writes the
// color that they compose, as one gesture of `picker` for each drag.
void planeRow(PrKit& kit, PrLayoutNode node, PickerState& s, PrColor* color, bool opaque) {
    bool perceptual = s.tab == PrKitColorMode_Oklch;
    PrLayoutNode row = part(kit, node, 0, Part_Row);
    Area plane = area(kit, row, Part_Plane);
    Area hues = area(kit, row, Part_Strip);
    Area alpha = opaque ? Area{} : area(kit, row, Part_Strip);
    if (plane.placed) {
        if (perceptual) {
            s.oklch.lightness = plane.at.x;
            s.oklch.chroma = (1 - plane.at.y) * kMaxChroma;
        } else {
            s.saturation = plane.at.x * 100;
            s.value = (1 - plane.at.y) * 100;
        }
    }
    if (hues.placed) {
        if (perceptual)
            s.oklch.hue = hues.at.y * 360;
        else
            s.hue = hues.at.y * 360;
    }
    if (alpha.placed)
        s.alpha = 1 - alpha.at.y;
    // The color is written only under a gesture, so that a resting picker never writes back a rounding of itself.
    if (plane.placed || hues.placed || alpha.placed) {
        if (!s.live) {
            s.live = true;
            prUiEdit(kit.ui, node, PrUiEditPhase_Begin);
        }
        PrColor made = composed(s);
        if (!sameColor(made, *color)) {
            *color = made;
            s.written = made;
            writeFields(s, made);
            prUiEdit(kit.ui, node, PrUiEditPhase_Change);
        }
    }
    if (!plane.active && !hues.active && !alpha.active && s.live) {
        s.live = false;
        prUiEdit(kit.ui, node, PrUiEditPhase_Commit);
    }

    PrVec2 planeExtent{kPlaneSize - 2, kPlaneSize - 2};
    PrVec2 stripExtent{kStripWidth - 2, kPlaneSize - 2};
    float hue = perceptual ? s.oklch.hue : s.hue;
    // A drawing is made again for another hue or mode, or on a display of another gamut.
    uint64_t gamutKey = uint64_t(s.gamut) << 8;
    uint64_t key = prHashBytes(&hue, sizeof hue, (s.tab + 1) | gamutKey);
    setDrawing(kit, plane.fill, planeExtent, key, &s.drawn[0], [&](PrDrawList* list) {
        if (perceptual) {
            drawSampled(kit, list, planeExtent, Sampled{.skyline = false, .hue = hue}, s.gamut);
            return;
        }
        PrRect rect{0, 0, planeExtent.x, planeExtent.y};
        fillGradient(list, rect, PrVec2{0, 0}, PrVec2{planeExtent.x, 0}, PrColor{1, 1, 1, 1}, srgbOfHsv(hue, 100, 100, 1));
        fillGradient(list, rect, PrVec2{0, 0}, PrVec2{0, planeExtent.y}, PrColor{0, 0, 0, 0}, PrColor{0, 0, 0, 1});
    });
    if (perceptual)
        marker(kit, plane.node, s.oklch.lightness, 1 - s.oklch.chroma / kMaxChroma, false);
    else
        marker(kit, plane.node, s.saturation / 100, 1 - s.value / 100, false);

    setDrawing(kit, hues.fill, stripExtent, (s.tab + 1) | gamutKey, &s.drawn[1], [&](PrDrawList* list) {
        constexpr uint32_t kStops = 13;
        PrDrawColorStop stops[kStops];
        for (uint32_t i = 0; i < kStops; ++i) {
            float place = float(i) / (kStops - 1);
            PrColor at;
            if (perceptual)
                at = shown(Oklch{kHueLightness, kHueChroma, place * 360}, 1, s.gamut);
            else
                at = srgbOfHsv(place * 360, 100, 100, 1);
            stops[i] = PrDrawColorStop{.offset = place, .color = at};
        }
        fillStops(list, PrRect{0, 0, stripExtent.x, stripExtent.y}, PrVec2{0, 0}, PrVec2{0, stripExtent.y}, stops, kStops);
    });
    marker(kit, hues.node, 0, hue / 360, true);

    if (!opaque) {
        PrColor solid{color->r, color->g, color->b, 1};
        setDrawing(kit, alpha.fill, stripExtent, prHashBytes(&solid, sizeof solid, 0), &s.drawn[2], [&](PrDrawList* list) {
            PrRect rect{0, 0, stripExtent.x, stripExtent.y};
            fillChecker(list, rect);
            PrColor clear = solid;
            clear.a = 0;
            fillGradient(list, rect, PrVec2{0, 0}, PrVec2{0, stripExtent.y}, solid, clear);
        });
        marker(kit, alpha.node, 0, 1 - clampUnit(s.alpha), true);
    }
}

// Creates the ramp's plane, whose drags set the ramp's hue and chroma without writing the color.
void rampPlane(PrKit& kit, PrLayoutNode node, PickerState& s) {
    PrLayoutNode row = part(kit, node, 0, Part_Row);
    Area plane = area(kit, row, Part_Wide);
    if (plane.placed) {
        s.rampHue = plane.at.x * 360;
        s.rampChroma = (1 - plane.at.y) * kMaxChroma;
    }
    PrVec2 extent{kPickerWidth - 2, kPlaneSize - 2};
    setDrawing(kit, plane.fill, extent, 0x5c71 | uint64_t(s.gamut) << 16, &s.drawn[0],
        [&](PrDrawList* list) { drawSampled(kit, list, extent, Sampled{.skyline = true}, s.gamut); });
    marker(kit, plane.node, s.rampHue / 360, 1 - s.rampChroma / kMaxChroma, false);
}

// Creates the ramp's tones, a click on which sets the color to the tone, keeping its alpha.
void rampTones(PrKit* kit, PrLayoutNode node, const PickerState& s, PrColor* color) {
    const PrKitPalette& p = kit->theme.palette;
    PrKitTextStyle caption{.role = PrKitTextRole_Caption};
    prKitLabel(kit, node, {.text = "TONAL RAMP"}, caption);
    PrLayoutNode row = part(*kit, node, 0, Part_Tones);
    for (uint32_t i = 0; i < kRampTones; ++i) {
        PrColor tone = rampTone(s.rampHue, s.rampChroma, i, s.gamut);
        bool chosen = channelByte(tone.r) == channelByte(color->r) && channelByte(tone.g) == channelByte(color->g) &&
            channelByte(tone.b) == channelByte(color->b);
        PrLayoutNode cell = prLayoutTreeFrameElement(kit->tree, row, 0, 0);
        prUiSetFlags(kit->ui, cell, PrUiFlag_Pressable | PrUiFlag_Focusable);
        bool hover = prUiIsHovered(kit->ui, cell);
        KitColors frame{.background = tone, .border = chosen || hover ? p.text1 : tone};
        prLayoutTreeFrameSetStyle(kit->tree, cell, kitStyle(*kit, ToneBox{.chosen = chosen}, KitGrow{}, frame));
        prUiSetCursor(kit->ui, cell, PrAppCursor_Hand);
        if (prUiActivated(kit->ui, cell))
            kitEditOnce(*kit, cell, [&] { *color = PrColor{tone.r, tone.g, tone.b, color->a}; });
    }
}

// Sets a sink on `row` that passes the gestures of the fields under it nowhere. The fields write the picker's own
// copies of its values, and the picker reports each edit of the color as a gesture of its own.
void quiet(PrKit& kit, PrLayoutNode row) {
    prUiSetEditSink(kit.ui, row, nullptr, nullptr);
}

// Writes `made` to the color as one gesture of `node`, if it differs, and reads it back into the picker.
void writeEdit(PrKit& kit, PrLayoutNode node, PickerState& s, PrColor* color, PrColor made) {
    if (!sameColor(made, *color))
        kitEditOnce(kit, node, [&] { *color = made; });
    s.written = made;
    readColor(s, made);
    writeFields(s, made);
}

// Creates the fields of the open mode's coordinates, and folds an edit of them back into the color.
void coordinateRow(PrKit* kit, PrLayoutNode node, PickerState& s, PrColor* color) {
    PrLayoutNode row = part(*kit, node, 0, Part_Row);
    PrKitNumberStyle small{.size = PrKitControlSize_Sm};
    bool perceptual = s.tab == PrKitColorMode_Oklch;
    quiet(*kit, row);
    if (perceptual) {
        PrKitNumber l{
            .value = prKitScalarF32(&s.lightness),
            .range = {.step = 0.5, .min = 0, .max = 100, .hasMin = true, .hasMax = true},
            .axis = "L",
            .precision = 1,
        };
        PrKitNumber c{
            .value = prKitScalarF32(&s.chroma),
            .range = {.step = 0.005, .min = 0, .max = double(kMaxChroma), .hasMin = true, .hasMax = true},
            .axis = "C",
            .precision = 3,
        };
        PrKitNumber h{
            .value = prKitScalarI32(&s.whole[2]),
            .range = {.step = 1, .min = 0, .max = 360, .hasMin = true, .hasMax = true},
            .axis = "H",
        };
        prKitNumberField(kit, row, l, small);
        prKitNumberField(kit, row, c, small);
        prKitNumberField(kit, row, h, small);
    } else {
        const PrStringView tags[] = {"H", "S", "V"};
        const double bounds[] = {360, 100, 100};
        for (uint32_t i = 0; i < 3; ++i) {
            PrKitNumber n{
                .value = prKitScalarI32(&s.whole[i]),
                .range = {.step = 1, .min = 0, .max = bounds[i], .hasMin = true, .hasMax = true},
                .axis = tags[i],
            };
            prKitNumberField(kit, row, n, small);
        }
    }
    bool changed = s.lightness != s.lightnessWritten || s.chroma != s.chromaWritten;
    for (uint32_t i = 0; i < 3; ++i)
        changed = changed || s.whole[i] != s.wholeWritten[i];
    if (!changed)
        return;
    if (perceptual) {
        s.oklch = Oklch{clampRange(s.lightness, 0, 100) / 100, clampRange(s.chroma, 0, kMaxChroma), float(s.whole[2])};
    } else {
        s.hue = float(s.whole[0]);
        s.saturation = clampRange(float(s.whole[1]), 0, 100);
        s.value = clampRange(float(s.whole[2]), 0, 100);
    }
    PrColor made = composed(s);
    if (!sameColor(made, *color))
        kitEditOnce(*kit, node, [&] { *color = made; });
    s.written = made;
    writeFields(s, made);
}

// Creates the swatch of the color beside its hex field, and folds an edit of the field back into the color.
void hexRow(PrKit* kit, PrLayoutNode node, PickerState& s, PrColor* color) {
    PrLayoutNode row = part(*kit, node, 0, Part_Row);
    prKitSwatch(kit, row, {.color = *color}, {.size = kPrKitHeightSm});
    // The text follows the color, except while the field has focus, where it is the text being typed.
    if (s.hexField == kPrLayoutNodeNone || !prUiIsFocusWithin(kit->ui, s.hexField)) {
        char hex[9];
        PrStringView text = prKitColorHex(*color, hex);
        for (uint32_t i = 0; i < text.size; ++i)
            s.hex[i] = text.data[i];
        s.hexLength = uint8_t(text.size);
    }
    PrKitTextField field{.text = PrStringView{s.hex, s.hexLength}, .selectOnFocus = true};
    PrKitFieldStyle mono{.size = PrKitControlSize_Sm, .mono = true};
    quiet(*kit, row);
    s.hexField = prKitTextField(kit, row, field, mono);
    PrStringView changed{};
    if (!prKitTextChanged(kit, s.hexField, &changed))
        return;
    s.hexLength = uint8_t(changed.size < sizeof s.hex - 1 ? changed.size : sizeof s.hex - 1);
    prMemCopy(s.hex, changed.data, s.hexLength);
    PrColor parsed{};
    if (prKitColorParseHex(PrStringView{s.hex, s.hexLength}, &parsed))
        writeEdit(*kit, node, s, color, parsed);
}

// Creates the channel fields, and folds an edit of them back into the color.
void channelRow(PrKit* kit, PrLayoutNode node, PickerState& s, PrColor* color, bool opaque) {
    PrLayoutNode row = part(*kit, node, 0, Part_Row);
    PrKitNumberStyle small{.size = PrKitControlSize_Sm};
    const PrStringView tags[] = {"R", "G", "B", "A"};
    quiet(*kit, row);
    for (uint32_t i = 0; i < (opaque ? 3u : 4u); ++i) {
        PrKitNumber n{
            .value = prKitScalarI32(&s.channels[i]),
            .range = {.step = 1, .min = 0, .max = 255, .hasMin = true, .hasMax = true},
            .axis = tags[i],
        };
        prKitNumberField(kit, row, n, small);
    }
    bool changed = false;
    for (uint32_t i = 0; i < 4; ++i)
        changed = changed || s.channels[i] != s.channelsWritten[i];
    if (!changed)
        return;
    PrColor made{
        float(s.channels[0]) / 255, float(s.channels[1]) / 255, float(s.channels[2]) / 255,
        float(s.channels[3]) / 255,
    };
    writeEdit(*kit, node, s, color, made);
}

PrLayoutNode colorPicker(PrKit* kit, PrLayoutNode parent, const PrKitColorPicker& picker) {
    PR_ASSERT(picker.color != nullptr && picker.mode <= PrKitColorMode_Ramp);
    PrColor* color = picker.color;
    PrLayoutNode node = part(*kit, parent, picker.key, Part_Picker);
    PickerState* state = static_cast<PickerState*>(prUiState(kit->ui, node, &kKitOwner, ColorStateKey_Picker,
        sizeof(PickerState)));
    PickerState& s = *state;
    s.gamut = picker.gamut;
    if (!s.seeded) {
        s.tab = s.seededTab = picker.mode;
        s.hexField = kPrLayoutNodeNone;
        readRamp(s, *color);
    }
    if (!s.seeded || !sameColor(*color, s.written)) {
        readColor(s, *color);
        writeFields(s, *color);
        s.written = *color;
        s.seeded = true;
    }

    // Switching the plane moves no color, so it is no gesture.
    static const PrStringView kModes[] = {"HSV", "OKLCH", "RAMP"};
    PrLayoutNode modes = prKitSegmented(kit, node, {}, {.size = PrKitControlSize_Sm});
    for (uint32_t mode = 0; mode < 3; ++mode) {
        if (prKitActivated(kit, prKitSegment(kit, modes, {.label = kModes[mode], .selected = s.tab == mode})))
            s.tab = mode;
    }
    if (s.tab != s.seededTab) {
        readColor(s, *color);
        if (s.tab == PrKitColorMode_Ramp)
            readRamp(s, *color);
        writeFields(s, *color);
        s.seededTab = s.tab;
    }

    if (s.tab == PrKitColorMode_Ramp) {
        rampPlane(*kit, node, s);
        rampTones(kit, node, s, color);
    } else {
        planeRow(*kit, node, s, color, picker.opaque);
        coordinateRow(kit, node, s, color);
    }
    hexRow(kit, node, s, color);
    channelRow(kit, node, s, color, picker.opaque);
    return node;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitColorField(PrKit* kit, PrLayoutNode parent, PrKitColorField field, PrKitColorFieldStyle style) {
    PR_ASSERT(field.color != nullptr);
    PrColor* color = field.color;
    PrUi* ui = kit->ui;
    PrLayoutNode node = prLayoutTreeFrameElement(kit->tree, parent, field.key, 0);
    PrUiFlags flags = PrUiFlag_Pressable | PrUiFlag_Focusable;
    bool disabled = kitSetFlags(*kit, node, field.disabled ? flags | PrUiFlag_Disabled : flags);
    uint8_t state = disabled || !prUiIsHovered(ui, node) ? 0 : 1;
    bool ring = prUiIsFocusVisible(ui) && prUiFocus(ui) == node;
    prLayoutTreeFrameSetStyle(kit->tree, node,
        kitStyle(*kit, FieldBox{.size = style.size}, PickerRowFlex{.gap = kPrKitSpace3}, RingFill{.ring = ring}));
    if (!disabled)
        prUiSetCursor(ui, node, PrAppCursor_Hand);
    if (!disabled && prUiActivated(ui, node))
        prKitToggleOpen(kit, node);
    PrLayoutNode popover = prKitPopover(kit, node, {}, {.offset = {0, kPrKitSpace2}});
    if (popover != kPrLayoutNodeNone) {
        // The field's picker works in the gamut of the display that it is shown on.
        PrKitColorPicker picker{.color = color, .gamut = prUiDisplay(ui).gamut};
        colorPicker(kit, popover, picker);
    }
    prKitSwatch(kit, node, {.color = *color}, {.size = kitControlHeight(style.size) - kPrKitSpace2});
    char hex[9];
    const PrKitPalette& p = kit->theme.palette;
    KitInk ink{.color = disabled ? p.text4 : state == 0 ? p.text2 : p.text1};
    PrLayoutNode label = prLayoutTreeFrameElement(kit->tree, node, 0,
        kitStyle(*kit, KitBlock{}, KitFont{.role = PrKitTextRole_Mono}, ink));
    prLayoutTreeFrameText(kit->tree, label, 0, prKitColorHex(*color, hex));
    return node;
}

PrLayoutNode prKitColorPicker(PrKit* kit, PrLayoutNode parent, PrKitColorPicker picker) {
    return colorPicker(kit, parent, picker);
}

PrStringView prKitColorHex(PrColor color, char* out) {
    static const char kDigits[] = "0123456789ABCDEF";
    const uint8_t channels[4] = {
        channelByte(color.r), channelByte(color.g), channelByte(color.b),
        channelByte(color.a),
    };
    uint32_t count = channels[3] == 255 ? 3 : 4;
    out[0] = '#';
    for (uint32_t i = 0; i < count; ++i) {
        out[1 + i * 2] = kDigits[channels[i] >> 4];
        out[2 + i * 2] = kDigits[channels[i] & 15];
    }
    return PrStringView{out, 1 + count * 2};
}

bool prKitColorParseHex(PrStringView text, PrColor* out) {
    if (text.size != 0 && text.data[0] == '#')
        text = PrStringView{text.data + 1, text.size - 1};
    if (text.size != 6 && text.size != 8)
        return false;
    uint32_t channels[4] = {0, 0, 0, 255};
    for (uint32_t i = 0; i < uint32_t(text.size) / 2; ++i) {
        char high = text.data[i * 2];
        char low = text.data[i * 2 + 1];
        if (!prAsciiIsHexDigit(high) || !prAsciiIsHexDigit(low))
            return false;
        channels[i] = prAsciiHexValue(high) * 16 + prAsciiHexValue(low);
    }
    *out = PrColor{
        float(channels[0]) / 255, float(channels[1]) / 255, float(channels[2]) / 255,
        float(channels[3]) / 255,
    };
    return true;
}
