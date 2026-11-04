// The scenes (scenes.h): animated pictures that combine what lib/draw does, each laid out for 960 by 720 and fitted to
// the size it is drawn at. Their text is shaped by the shaper that scenesOpen gives, in fonts of the Noto bundle.

#include "examples/draw/scenes.h"

#include "pr/array.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/locale.h"
#include "pr/min_max.h"
#include "pr/print.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text.h"
#include "pr/utf8.h"

namespace scenes {

using namespace pr;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr PrColor kWhite{1, 1, 1, 1};

// ---------------------------------------------------------------------------------------------------------------------
// Text

PrTextShaper* textShaper = nullptr;
String fontDirectory;
PrTextGlyphBuffer* glyphBuffer = nullptr;

// A font of the bundle at a design point, loaded once for each.
struct LoadedFont {
    String key;
    const PrFont* font;
};

Array<LoadedFont>& loadedFonts() {
    static Array<LoadedFont> fonts;
    return fonts;
}

// Returns the font of the bundle's file `file` at the design point that `values` give, loaded once for each. Returns
// null if the file does not load.
const PrFont* PR_NULLABLE fontVaried(const char* file, const PrFontVariation* PR_NULLABLE values, uint32_t count) {
    String key(file);
    for (uint32_t i = 0; i < count; ++i)
        formatTo(key, " {}={}", values[i].tag, values[i].value);
    for (const LoadedFont& loaded : loadedFonts()) {
        if (loaded.key == key)
            return loaded.font;
    }
    const PrFont* font = nullptr;
    if (count != 0) {
        const PrFont* base = fontVaried(file, nullptr, 0);
        PrFontDerivation derivation = prFontDerivationDefault();
        derivation.variations = values;
        derivation.variationCount = count;
        font = base != nullptr ? prFontCreateDerived(base, &derivation) : nullptr;
    } else {
        // The font copies the file's bytes, so the mapping goes as soon as the font is made.
        PrFileMapping mapping = {};
        String path = format("{}/{}", fontDirectory, file);
        bool mapped = prFileMap(path, &mapping);
        if (!mapped || prFontCreate(PrByteView{mapping.data, mapping.size}, 0, nullptr, &font) != PrStatus_Ok)
            font = nullptr;
        prFileUnmap(&mapping);
    }
    loadedFonts().push(LoadedFont{key, font});
    return font;
}

const PrFont* PR_NULLABLE fontNamed(const char* file) {
    return fontVaried(file, nullptr, 0);
}

// How a run of text is shaped and drawn.
struct TextStyle {
    const PrFont* PR_NULLABLE font = nullptr;
    float size = 16;
    PrDrawPaint paint{.color = PrColor{0, 0, 0, 1}};
    PrTextAxis axis = PrTextAxis_Horizontal;
    const char* PR_NULLABLE language = nullptr;     // BCP 47
    const PrTextFeature* PR_NULLABLE features = nullptr;
    uint32_t featureCount = 0;
};

// Glyphs placed for one run in a style's font, and a transform for each that the shaper synthesized at another size, or
// none if every glyph is drawn at the style's size.
struct ShapedText {
    Array<PrFontGlyph> glyphs;
    Array<PrAffine2> transforms;
};

// Places the text's glyphs on the baseline from `origin`, left to right or top to bottom. Returns the advance.
float shapeText(const TextStyle& style, PrStringView text, PrVec2 origin, ShapedText& out) {
    out.glyphs.clear();
    out.transforms.clear();
    if (style.font == nullptr)
        return 0;
    PrTextRun run{};
    run.font = style.font;
    run.text = text;
    run.axis = style.axis;
    if (style.language != nullptr)
        run.locale = prLocaleFromTag(style.language);
    run.features = style.features;
    run.featureCount = style.featureCount;
    run.pixelsPerEm = style.size;
    if (prTextShaperShape(textShaper, &run, glyphBuffer) != PrStatus_Ok)
        return 0;
    uint32_t count = prTextGlyphBufferGlyphCount(glyphBuffer);
    out.glyphs.resize(count);
    out.transforms.resize(count);
    PrVec2 pen = prTextGlyphBufferPlace(glyphBuffer, style.size, origin, out.glyphs.data(), out.transforms.data());
    bool scaled = false;
    for (const PrAffine2& transform : out.transforms)
        scaled = scaled || transform.a != 1;
    if (!scaled)
        out.transforms.clear();
    return style.axis == PrTextAxis_Vertical ? pen.y - origin.y : pen.x - origin.x;
}

float textAdvance(const TextStyle& style, PrStringView text) {
    ShapedText shaped;
    return shapeText(style, text, PrVec2{0, 0}, shaped);
}

// Records the glyphs as one run in the style's font.
void drawGlyphs(PrDrawList* list, const TextStyle& style, const ShapedText& shaped) {
    if (shaped.glyphs.isEmpty())
        return;
    PrFontGlyphRun run{};
    run.font = style.font;
    run.size = style.size;
    run.glyphs = shaped.glyphs.data();
    run.glyphCount = shaped.glyphs.count();
    run.transforms = shaped.transforms.isEmpty() ? nullptr : shaped.transforms.data();
    prDrawListGlyphs(list, &run, PrVec2{}, style.paint);
}

// Records the text as one glyph run from `origin`. Returns its advance.
float drawText(PrDrawList* list, const TextStyle& style, PrStringView text, PrVec2 origin) {
    ShapedText shaped;
    float advance = shapeText(style, text, origin, shaped);
    drawGlyphs(list, style, shaped);
    return advance;
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing

// Returns a default stroke of `width`, with `join`, and with `cap` at both ends and on dashes.
PrDrawStroke strokeOf(float width, PrPathJoin join = PrPathJoin_Miter, PrPathCap cap = PrPathCap_Butt) {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.shape.join = join;
    stroke.shape.startCap = cap;
    stroke.shape.endCap = cap;
    stroke.shape.dashCap = cap;
    return stroke;
}

// Records the image's texels in `source` drawn over `destination`, at `opacity`, as a rectangle filled with the image's
// brush.
void drawImage(PrDrawList* list, const PrDrawImage* image, PrRect source, PrRect destination, PrDrawSampling sampling,
        float opacity) {
    PrDrawSource from = prDrawSourceImage(image, source);
    PrDrawBrush brush = prDrawBrushImage(from, destination, sampling);
    brush.opacity = opacity;
    prDrawListRect(list, destination, {.brush = &brush});
}

// ---------------------------------------------------------------------------------------------------------------------
// The scenes

constexpr float kSceneWidth = 960;
constexpr float kSceneHeight = 720;

constexpr PrColor kGalleryInk{.075f, .13f, .14f, 1};
constexpr PrColor kMuted{.40f, .46f, .44f, 1};
constexpr PrColor kCoral{.94f, .30f, .20f, 1};
constexpr PrColor kMint{.63f, .79f, .63f, 1};
constexpr PrColor kCyan{.13f, .91f, 1, 1};
constexpr PrColor kViolet{.63f, .25f, 1, 1};
constexpr PrColor kHot{1, .21f, .47f, 1};

// The koi's colorings, each multiplying the koi's own colors.
enum {
    FishColoring_Plain,
    FishColoring_Gold,
    FishColoring_Shadow,
    FishColoring_Count,
};
typedef uint8_t FishColoring;

constexpr PrColor kFishColorings[FishColoring_Count] = {{1, 1, 1, 1}, {1, .84f, .55f, 1}, {.015f, .075f, .06f, .24f}};

const PrDrawImage* cachedFish[FishColoring_Count] = {};
const PrDrawImage* cachedWater = nullptr;
const PrDrawImage* cachedSkin = nullptr;

PrColor fade(PrColor color, float alpha) {
    color.a *= alpha;
    return color;
}

PrVec2 orbit(PrVec2 center, float radius, float angle) {
    return PrVec2{center.x + radius * prF32Cos(angle), center.y + radius * prF32Sin(angle)};
}

PrAffine2 turn(float angle, PrVec2 at, float sx = 1, float sy = 1) {
    float c = prF32Cos(angle);
    float s = prF32Sin(angle);
    return PrAffine2{c * sx, s * sx, -s * sy, c * sy, at.x, at.y};
}

const PrDrawImage* imageOf(const Array<uint8_t>& premultiplied, uint32_t width, uint32_t height) {
    PrDrawImageDesc desc{
        PrPixelView{premultiplied.data(), size_t(width) * 4, {width, height}}, PrPixelFormat_Rgba8,
        PrDrawAlpha_Premultiplied, PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    prDrawImageCreate(&desc, &image);
    return image;
}

void put(Array<uint8_t>& pixels, uint32_t width, uint32_t x, uint32_t y, PrColor c) {
    uint8_t* p = pixels.data() + (size_t(y) * width + x) * 4;
    p[0] = uint8_t(c.r * c.a * 255);
    p[1] = uint8_t(c.g * c.a * 255);
    p[2] = uint8_t(c.b * c.a * 255);
    p[3] = uint8_t(c.a * 255);
}

// Returns a koi seen from above, 64 by 64, in `coloring`.
const PrDrawImage* fish(FishColoring coloring = FishColoring_Plain) {
    if (cachedFish[coloring] != nullptr)
        return cachedFish[coloring];
    PrColor by = kFishColorings[coloring];
    Array<uint8_t> pixels(64 * 64 * 4);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            float dx = float(x) - 31.5f;
            float dy = float(y) - 26;
            bool body = dx * dx / 81 + dy * dy / 400 < 1;
            bool tail =
                y >= 43 && y <= 59 && prF32Abs(dx) < float(y - 40) * .63f && (y < 53 || prF32Abs(dx) > float(y - 53) * .8f);
            bool fins = y >= 25 && y <= 36 && prF32Abs(dx) < float(17 - (y > 29 ? y - 29 : 29 - y) * 2);
            PrColor color{0, 0, 0, 0};
            if (tail || fins)
                color = PrColor{.81f, .84f, .69f, .8f};
            if (body) {
                color = PrColor{.98f, .94f, .81f, 1};
                if ((x - 29) * (x - 29) + (y - 16) * (y - 16) < 36 || (x - 35) * (x - 35) + (y - 29) * (y - 29) < 44 ||
                        (x - 28) * (x - 28) + (y - 37) * (y - 37) < 20)
                    color = PrColor{.89f, .25f, .09f, 1};
                if (y > 26 && (x + y * 2) % 9 == 0)
                    color = PrColor{color.r * .89f, color.g * .89f, color.b * .89f, 1};
                if ((x == 26 || x == 37) && y == 15)
                    color = PrColor{.09f, .14f, .12f, 1};
            }
            color = PrColor{color.r * by.r, color.g * by.g, color.b * by.b, color.a * by.a};
            put(pixels, 64, uint32_t(x), uint32_t(y), color);
        }
    }
    cachedFish[coloring] = imageOf(pixels, 64, 64);
    return cachedFish[coloring];
}

// Returns faint wavy lines on transparency, 32 by 32, which tile.
const PrDrawImage* water() {
    if (cachedWater != nullptr)
        return cachedWater;
    Array<uint8_t> pixels(32 * 32 * 4);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            float wave = prF32Sin(float(x) * kPi / 16) * 2;
            bool line = (y + int(wave + 3)) % 16 == 0;
            uint8_t* p = pixels.data() + size_t(y * 32 + x) * 4;
            p[0] = line ? 25 : 0;
            p[1] = line ? 46 : 0;
            p[2] = line ? 37 : 0;
            p[3] = line ? 52 : 0;
        }
    }
    cachedWater = imageOf(pixels, 32, 32);
    return cachedWater;
}

// Returns a gilded frame, 32 by 32, for a nine-slice.
const PrDrawImage* skin() {
    if (cachedSkin != nullptr)
        return cachedSkin;
    Array<uint8_t> pixels(32 * 32 * 4);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            int edge = min(min(x, y), min(31 - x, 31 - y));
            PrColor color{.30f, .14f, .07f, 1};
            if (edge == 1 || edge == 5)
                color = PrColor{.98f, .81f, .48f, 1};
            else if (edge > 1 && edge < 5)
                color = PrColor{.72f, .46f, .19f, 1};
            else if (edge == 7)
                color = PrColor{.87f, .65f, .31f, 1};
            if (edge == 3 && (x + y) % 4 == 0)
                color = PrColor{1, .91f, .70f, 1};
            put(pixels, 32, uint32_t(x), uint32_t(y), color);
        }
    }
    cachedSkin = imageOf(pixels, 32, 32);
    return cachedSkin;
}

// The stage that a scene draws on: the list, the time, the part of the scene's space that the window shows, and the
// pointer in that space. The view holds the scene's 960 by 720, and reaches past it where the drawing's shape differs,
// and over the caption's band, so a scene's backdrop fills the view.
struct Stage {
    PrDrawList* list;
    float t;
    PrRect view;
    PrVec2 at;                  // the pointer, or one moving over the scene of its own while the pointer is elsewhere
    float hold;                 // from 0 to 1 as the primary button is held

    // Fills the view.
    void fill(PrColor color) const {
        prDrawListRect(list, view, {.color = color});
    }

    // Calls `draw` with the left edge of each copy of a band `width` wide, laid along x from the scene's origin, that
    // the view shows.
    template <typename Draw>
    void across(float width, Draw draw) const {
        for (float x = prF32Floor(view.left / width) * width; x < view.right; x += width)
            draw(x);
    }

    // Calls `draw` with the corner of each copy of a tile of `size`, laid from the scene's origin, that the view shows.
    template <typename Draw>
    void tiles(PrVec2 size, Draw draw) const {
        for (float y = prF32Floor(view.top / size.y) * size.y; y < view.bottom; y += size.y)
            across(size.x, [&](float x) { draw(PrVec2{x, y}); });
    }

    // Returns the x at which something that wraps around the view is drawn: `travel` along a loop through the view and
    // `margin` past each of its sides.
    float wrap(float travel, float margin) const {
        return view.left - margin + prF32Fmod(travel, view.right - view.left + 2 * margin);
    }

    void label(PrStringView text, float x, float y, float size, PrColor tint = kGalleryInk) const {
        TextStyle style;
        style.font = fontNamed("NotoSans[wdth,wght].ttf");
        style.size = size;
        style.paint.color = tint;
        drawText(list, style, text, PrVec2{x, y});
    }

    void rect(float x, float y, float w, float h, PrColor color, float radius = 0) const {
        PrRect r{x, y, x + w, y + h};
        if (radius <= 0) {
            prDrawListRect(list, r, {.color = color});
            return;
        }
        PrCornerRect rr = prCornerRectRound(r, radius);
        prDrawListCornerRect(list, rr, {.color = color});
    }

    void capsule(PrVec2 a, PrVec2 b, float radius, PrColor color) const {
        prDrawListCapsule(list, a, b, radius, {.color = color});
    }

    // Draws a band of the ring between `radius - width` and `radius`, from `start` through `span` radians.
    void ring(PrVec2 center, float radius, float width, float start, float span, PrColor color) const {
        prDrawListSector(list, center, max(0.0f, radius - width), radius, start, span, {.color = color});
    }

    // Fills the polygon of the `count` points at `points`, or of an array of points, or of a list of them, as in
    // `s.polygon({{0, 0}, {10, 0}, {5, 8}}, color)`.
    void polygon(const PrVec2* points, uint32_t count, PrColor color) const {
        prDrawListPolylineContour(list, points, count, PrDrawContour_Closed, PrPathFillRule_NonZero, {.color = color});
    }
    void polygon(const Array<PrVec2>& points, PrColor color) const { polygon(points.data(), points.count(), color); }
    template <uint32_t N>
    void polygon(const PrVec2 (&points)[N], PrColor color) const {
        polygon(points, N, color);
    }

    void beam(PrVec2 a, PrVec2 b, float radius, PrColor color) const {
        capsule(a, b, radius * 6, fade(color, .025f));
        capsule(a, b, radius * 3, fade(color, .07f));
        capsule(a, b, radius, color);
    }

    void glow(PrVec2 center, float radius, PrColor color) const {
        // A Gaussian falloff, exp(-5 r^2) less its value at the edge, in stops.
        PrDrawColorStop stops[6];
        for (uint32_t i = 0; i < 6; ++i) {
            float r = float(i) / 5;
            float alpha = (prF32Exp(-5 * r * r) - prF32Exp(-5.0f)) / (1 - prF32Exp(-5.0f));
            stops[i] = PrDrawColorStop{.offset = r, .color = fade(color, .6f * alpha)};
        }
        PrDrawBrush brush = prDrawBrushRadial(center, 0, center, radius, stops, 6);
        prDrawListEllipse(list, center, PrVec2{radius, radius}, 0, {.brush = &brush});
    }

    void push(PrAffine2 transform) const {
        prDrawListPushTransformAffine2(list, transform);
    }

    void pop() const {
        prDrawListPopTransform(list);
    }
};

// How the scene's space sits in a drawing: scaled by `scale`, then moved by `offset`.
struct Placement {
    float scale;
    PrVec2 offset;
};

// Places the scene's space to fit the drawing above the caption's band, centered there.
Placement fit(float width, float height) {
    float above = max(height - kCaptionBand, 1.0f);
    float scale = min(width / kSceneWidth, above / kSceneHeight);
    return Placement{scale, PrVec2{(width - kSceneWidth * scale) / 2, (above - kSceneHeight * scale) / 2}};
}

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

void gauges(const Stage& s) {
    const PrVec2 center{300, 398};
    s.fill(PrColor{.953f, .945f, .91f, 1});
    s.label("FIELD NOTES / 01", 48, 56, 12, kMuted);
    s.label("Signal & motion", 48, 110, 38);
    s.label("A little further, every day.", 48, 145, 16, kMuted);
    for (int i = 0; i < 60; ++i) {
        float angle = float(i) * kPi / 30;
        float inner = i % 5 == 0 ? 198.0f : 204.0f;
        s.capsule(orbit(center, inner, angle), orbit(center, 212, angle), i % 5 == 0 ? 1.4f : .6f, kMuted);
    }
    const PrColor colors[] = {kCoral, kGalleryInk, kMint};
    const float amounts[] = {
        .78f + .08f * prF32Sin(s.t),
        .64f + .1f * prF32Sin(s.t * .7f),
        .89f + .04f * prF32Cos(s.t),
    };
    for (int i = 0; i < 3; ++i) {
        float outer = 184.0f - float(i) * 30;
        s.ring(center, outer, 20, -kPi / 2, 2 * kPi, PrColor{.85f, .86f, .81f, 1});
        s.ring(center, outer, 20, -kPi / 2, 2 * kPi * amounts[i], colors[i]);
        float radius = outer - 10;
        float end = -kPi / 2 + 2 * kPi * amounts[i];
        PrVec2 top{center.x, center.y - radius};
        PrVec2 tip = orbit(center, radius, end);
        s.capsule(top, top, 10, colors[i]);
        s.capsule(tip, tip, 10, colors[i]);
    }
    s.label(format("{:02}", int(amounts[0] * 100)), 242, 415, 74);
    s.label("DAILY MOMENTUM", 240, 447, 11, kMuted);
    s.rect(586, 200, 326, 424, kGalleryInk, 28);
    s.label("THIS WEEK", 616, 244, 12, kMint);
    s.label("Steady is strong.", 616, 288, 25, kWhite);
    const float bars[] = {88, 128, 104, 174, 143, 192, 164};
    const char* days[] = {"M", "T", "W", "T", "F", "S", "S"};
    for (int i = 0; i < 7; ++i) {
        float x = 630 + float(i) * 39;
        s.capsule(PrVec2{x, 534}, PrVec2{x, 534 - bars[i] - 12 * prF32Sin(s.t * 1.3f + float(i))}, 10, i == 5 ? kCoral : kMint);
        s.label(days[i], x - 5, 569, 12, kWhite);
    }
    s.label("04 : 32", 616, 604, 16, kMint);
    s.label("TIME OUTSIDE", 740, 604, 11, kMint);
    s.label("MOVE", 103, 665, 12, kCoral);
    s.label("FOCUS", 259, 665, 12);
    s.label("REST", 425, 665, 12, kMuted);
}

// Draws a frame of the gilded nine-slice around the area.
void theatreFrame(const Stage& s, PrRect area) {
    PrDrawNineSlice slices{{8, 8, 8, 8}, {18, 18, 18, 18}, PrDrawSliceMode_Repeat, PrDrawSliceMode_Repeat, false};
    PrDrawSource source = prDrawSourceImage(skin(), PrRect{0, 0, 32, 32});
    prDrawListNineSlice(s.list, source, &slices, area, PrDrawSampling_Linear, {.color = kWhite});
}

void theatre(const Stage& s) {
    const float t = s.t;
    const PrColor velvet{.26f, .035f, .095f, 1};
    const PrColor gold{.87f, .65f, .31f, 1};
    const PrColor cream{1, .91f, .70f, 1};
    const PrColor night{.105f, .065f, .15f, 1};
    s.fill(velvet);
    s.across(30, [&](float x) { s.rect(x, s.view.top, 1, s.view.bottom - s.view.top, fade(gold, .10f)); });
    s.label("THE ORPHEUM / EST. 1926", 48, 41, 11, gold);
    s.label("PAPER THEATRE", 44, 104, 55, cream);
    s.label("A SMALL STAGE. AN IMPOSSIBLE NIGHT.", 48, 135, 11, gold);
    float w = 720 + 24 * prF32Sin(t * .43f);
    float h = 385 + 12 * prF32Cos(t * .57f);
    PrRect stage{480 - w / 2, 172, 480 + w / 2, 172 + h};
    s.rect(stage.left + 8, stage.top + 12, w, h, PrColor{.12f, .025f, .055f, 1});
    s.rect(stage.left, stage.top, w, h, night);
    prDrawListPushClipRect(s.list, stage, PrDrawEdges_Antialiased);
    for (int i = 0; i < 53; ++i) {
        PrVec2 point{170 + float((i * 113) % 640), 197 + float((i * 67) % 266)};
        s.capsule(point, point, .6f + .5f * (1 + prF32Sin(t + float(i))), fade(cream, .45f));
    }
    for (int i = 0; i < 3; ++i) {
        float x = 288 + float(i) * 192 + 18 * prF32Sin(t * .5f + float(i));
        s.polygon({{x, 178}, {x - 125, 540}, {x + 155, 540}}, fade(i == 1 ? cream : gold, .035f));
    }
    float swing = .13f * prF32Sin(t * .7f);
    PrVec2 moon{468 + 43 * prF32Sin(swing), 322 + 5 * prF32Cos(t * .7f)};
    s.capsule(PrVec2{480, 182}, moon, .65f, fade(gold, .6f));
    s.push(turn(swing, moon));
    PrPath* crescent = prPathCreate();
    prPathMoveTo(crescent, PrVec2{19, -79});
    prPathCubicTo(crescent, PrVec2{-103, -99}, PrVec2{-116, 91}, PrVec2{18, 84});
    prPathCubicTo(crescent, PrVec2{-53, 52}, PrVec2{-45, -45}, PrVec2{19, -79});
    prPathClose(crescent);
    prDrawListPath(s.list, crescent, PrPathFillRule_NonZero, {.color = cream});
    prPathDestroy(crescent);
    s.ring(PrVec2{-47, -4}, 9, 1.6f, .1f, kPi - .2f, velvet);
    s.capsule(PrVec2{-52, 26}, PrVec2{-45, 27}, 3.5f, PrColor{.88f, .47f, .37f, 1});
    s.pop();
    for (int i = 0; i < 5; ++i) {
        float x = 228 + float(i) * 123 + 13 * prF32Sin(t * .8f + float(i));
        float y = 247 + float((i * 43) % 130) + 10 * prF32Sin(t + float(i));
        s.capsule(PrVec2{x - 9 * prF32Sin(t * .8f), 177}, PrVec2{x, y}, .55f, fade(gold, .5f));
        Array<PrVec2> points;
        for (int tip = 0; tip < 10; ++tip)
            points.push(orbit(PrVec2{x, y}, tip % 2 ? 8.0f : 20.0f,
                float(tip) * kPi / 5 - kPi / 2 + .12f * prF32Sin(t + float(i))));
        s.polygon(points, i % 2 ? cream : gold);
    }
    const PrColor hills[] = {{.28f, .24f, .34f, 1}, {.44f, .36f, .44f, 1}, {.64f, .51f, .53f, 1}};
    for (int layer = 0; layer < 3; ++layer) {
        float y = 468 + float(layer) * 29;
        for (int cloud = 0; cloud < 6; ++cloud) {
            float x = 100 + float(cloud) * 150 + 14 * prF32Sin(t * .5f + float(layer));
            prDrawListEllipse(s.list, PrVec2{x, y}, PrVec2{110, 32 + float(cloud % 3) * 9}, 0, {.color = hills[layer]});
        }
        s.rect(120, y, 740, 100, hills[layer]);
    }
    for (int bird = 0; bird < 3; ++bird) {
        float phase = t * .45f + float(bird) * 1.9f;
        float x = 488 + 169 * prF32Sin(phase);
        float y = 429 + 19 * prF32Cos(phase * 1.3f);
        float flap = 24 + 18 * prF32Sin(t * 2.6f + float(bird));
        s.polygon({
            {x - 4, y + 4},
            {x - 42, y - flap},
            {x - 21, y + 6},
            {x, y + 12},
            {x + 36, y - flap * .7f},
            {x + 18, y + 3},
            {x + 8, y},
        }, cream);
        s.polygon({{x + 6, y}, {x + 21, y - 7}, {x + 15, y + 2}}, gold);
    }
    float opening = 17 + 17 * prF32Sin(t * .3f);
    for (int side = 0; side < 2; ++side) {
        float direction = side == 0 ? 1.0f : -1.0f;
        float edge = side == 0 ? stage.left : stage.right;
        for (int fold = 6; fold >= 0; --fold) {
            float width = 22 + float(fold) * 15 - opening * float(fold) / 6;
            Array<PrVec2> curtain({{edge, 174}, {edge + direction * width * 1.55f, 174}});
            for (int row = 0; row <= 24; ++row) {
                float y = 180 + float(row) * 17;
                float bulge = 1.1f - .48f * prF32Sin(float(row) / 24 * kPi);
                curtain.push({edge + direction * (width * bulge + 4 * prF32Sin(t * .8f + float(row) * .1f)), y});
            }
            curtain.push({edge, 590});
            s.polygon(curtain, fold % 2 ? PrColor{.48f, .055f, .13f, 1} : PrColor{.65f, .105f, .18f, 1});
        }
        float tieX = edge + direction * 49;
        s.capsule(PrVec2{edge, 389}, PrVec2{tieX, 398}, 3, gold);
        s.capsule(PrVec2{tieX, 398}, PrVec2{tieX + direction * 9, 430}, 2, gold);
        s.capsule(PrVec2{tieX + direction * 9, 430}, PrVec2{tieX + direction * 9, 438}, 4, cream);
    }
    for (int i = 0; i < 9; ++i) {
        PrVec2 center{stage.left + 45 + float(i) * (w - 90) / 8, 174};
        prDrawListEllipse(s.list, center, PrVec2{62, 43}, 0, {.color = gold});
        prDrawListEllipse(s.list, PrVec2{center.x, center.y - 3}, PrVec2{62, 42}, 0, {.color = velvet});
    }
    prDrawListPopClip(s.list);
    theatreFrame(s, stage);
    for (int i = 0; i < 23; ++i) {
        PrVec2 bulb{stage.left + 25 + float(i) * (w - 50) / 22, stage.bottom + 17};
        float pulse = prF32Sin(t * 1.2f + float(i) * .3f);
        s.capsule(bulb, bulb, 2.8f, fade(cream, .55f + .45f * pulse * pulse));
    }
    s.label("ACT II", 48, 622, 13, gold);
    s.label("THE MOON KEEPS ITS OWN TIME", 48, 645, 10, cream);
    float ticketWidth = 264 + 12 * prF32Sin(t * .7f);
    s.rect(634, 601, ticketWidth, 49, velvet);
    theatreFrame(s, PrRect{634, 601, 634 + ticketWidth, 650});
    s.label("TONIGHT AT EIGHT", 659, 631, 11, cream);
}

void koi(const Stage& s) {
    const float t = s.t;
    const PrColor leaf{.13f, .30f, .24f, 1};
    const PrColor linen{.91f, .92f, .81f, 1};
    const PrColor pondColor{.12f, .37f, .33f, 1};
    const PrColor vermilion{.86f, .23f, .12f, 1};
    s.fill(linen);
    s.tiles(PrVec2{950, 678}, [&](PrVec2 corner) {
        for (int i = 0; i < 80; ++i) {
            PrVec2 p = corner + PrVec2{float((i * 163) % 950), float((i * 79) % 678)};
            s.capsule(p, p, .7f, fade(leaf, .14f));
        }
    });
    const PrVec2 pond{374, 407};
    prDrawListEllipse(s.list, PrVec2{pond.x + 3, pond.y + 9}, PrVec2{343, 244}, -.06f,
        {.color = PrColor{.68f, .74f, .62f, 1}});
    prDrawListEllipse(s.list, pond, PrVec2{337, 237}, -.06f, {.color = pondColor});
    // Ripples: the water tiled at twice its size, drifting.
    PrDrawSource ripples = prDrawSourceImage(water(), PrRect{0, 0, 32, 32});
    PrDrawBrush brush = prDrawBrushImage(ripples, PrRect{0, 0, 32, 32}, PrDrawSampling_Nearest);
    brush.image.tiles.x = PrDrawExtend_Repeat;
    brush.image.tiles.y = PrDrawExtend_Repeat;
    brush.transform = PrAffine2{2, 0, 0, 2, t * 2, t * 3};
    brush.opacity = .45f;
    prDrawListEllipse(s.list, pond, PrVec2{330, 230}, -.06f, {.brush = &brush});
    struct Route {
        PrVec2 center;
        PrVec2 reach;
        float phase;
        float speed;
        float scale;
    };
    const Route routes[] = {
        {{365, 280}, {90, 26}, .4f, .18f, .95f},
        {{530, 397}, {40, 73}, 1.7f, -.14f, 1.1f},
        {{389, 535}, {93, 27}, 4.4f, -.16f, .86f},
        {{242, 431}, {48, 65}, 2.8f, .13f, 1.03f},
    };
    PrVec2 focus{};
    for (int i = 0; i < 4; ++i) {
        const Route& route = routes[i];
        float phase = route.phase + route.speed * (t + .7f * prF32Sin(t * .27f + route.phase));
        PrVec2 p{
            route.center.x + route.reach.x * (prF32Sin(phase) + .13f * prF32Sin(2 * phase)),
            route.center.y + route.reach.y * prF32Sin(2 * phase + .4f),
        };
        if (i == 0)
            focus = p;
        float dx = route.speed * route.reach.x * (prF32Cos(phase) + .26f * prF32Cos(2 * phase));
        float dy = route.speed * route.reach.y * 2 * prF32Cos(2 * phase + .4f);
        float angle = prF32Atan2(dy, dx) + kPi / 2;
        s.push(turn(angle, p, route.scale, route.scale));
        drawImage(s.list, fish(FishColoring_Shadow), PrRect{0, 0, 64, 64}, PrRect{-65, -57, 75, 83},
            PrDrawSampling_Nearest, 1);
        drawImage(s.list, fish(i % 3 == 1 ? FishColoring_Gold : FishColoring_Plain), PrRect{0, 0, 64, 64},
            PrRect{-70, -70, 70, 70}, PrDrawSampling_Nearest, 1);
        s.pop();
        float wake = prF32Fmod(t * .24f + float(i) * .13f, 1.0f);
        PrDrawStroke stroke = strokeOf(1);
        prDrawListEllipse(s.list, PrVec2{p.x - 10, p.y + 12}, PrVec2{17 + wake * 28, 8 + wake * 15}, angle,
            {.color = fade(linen, (1 - wake) * .12f), .stroke = &stroke});
    }
    for (int i = 0; i < 6; ++i) {
        PrVec2 p{128 + float((i * 71) % 156), 332 + float((i * 47) % 204) + 3 * prF32Sin(t * .6f + float(i))};
        float radius = 25 + float(i % 3) * 8;
        s.capsule(PrVec2{p.x + 4, p.y + 5}, PrVec2{p.x + 4, p.y + 5}, radius, fade(kGalleryInk, .13f));
        prDrawListSector(s.list, p, 0, radius, -.55f, 2 * kPi - .25f,
            {.color = i % 2 ? PrColor{.47f, .60f, .30f, 1} : PrColor{.32f, .51f, .28f, 1}});
        for (int vein = 0; vein < 7; ++vein)
            s.capsule(p, orbit(p, radius * .84f, .1f + float(vein) * .8f), .45f, fade(linen, .2f));
        if (i == 1 || i == 4) {
            for (int petal = 0; petal < 7; ++petal) {
                float angle = float(petal) * 2 * kPi / 7 + t * .025f;
                prDrawListEllipse(s.list, orbit(p, 9, angle), PrVec2{11, 5}, angle, {.color = PrColor{1, .76f, .68f, 1}});
            }
            s.capsule(p, p, 4, PrColor{1, .78f, .25f, 1});
        }
    }
    s.label("KOI GARDEN", 40, 85, 53, leaf);
    s.label("SLOW DOWN. LOOK A LITTLE CLOSER.", 44, 118, 11, leaf);
    s.rect(44, 137, 36, 5, vermilion);
    s.label("04", 848, 257, 42, leaf);
    s.label("RESIDENTS", 836, 279, 10, leaf);
    const PrVec2 lens{792, 433};
    for (int i = 0; i < 22; ++i) {
        float f = float(i) / 22;
        PrVec2 p{focus.x + (lens.x - 108 - focus.x) * f, focus.y + (lens.y - focus.y) * f};
        s.capsule(p, p, 1, fade(linen, .6f));
    }
    s.ring(focus, 48, 1, 0, 2 * kPi, fade(linen, .6f));
    s.capsule(PrVec2{862, 518}, PrVec2{902, 566}, 13, leaf);
    s.capsule(PrVec2{863, 517}, PrVec2{899, 560}, 4, PrColor{.37f, .48f, .31f, 1});
    s.capsule(lens, lens, 120, leaf);
    s.capsule(lens, lens, 114, PrColor{.71f, .65f, .42f, 1});
    s.capsule(lens, lens, 108, pondColor);
    // The lens magnifies part of a fish, spread over its bounds.
    PrDrawSource detail = prDrawSourceImage(fish(), PrRect{17 + 2 * prF32Sin(t * .4f), 7 + 2 * prF32Cos(t * .5f), 0, 0});
    detail.rect.right = detail.rect.left + 28;
    detail.rect.bottom = detail.rect.top + 28;
    PrDrawBrush magnified = prDrawBrushImage(detail, PrRect{0, 0, 1, 1}, PrDrawSampling_Nearest);
    magnified.space = PrDrawBrushSpace_ObjectBounds;
    prDrawListEllipse(s.list, lens, PrVec2{108, 108}, 0, {.brush = &magnified});
    s.ring(lens, 103, 2, -2.8f, .7f, fade(linen, .65f));
    s.label("THE SMALL THINGS / 8x", 663, 594, 11, leaf);
    s.label("WATER STUDY / 02", 44, 647, 10, leaf);
}

// Draws a panel with two corners cut off.
void cutPanel(const Stage& s, PrRect box, PrColor color) {
    float x = box.left;
    float y = box.top;
    float r = box.right;
    float b = box.bottom;
    s.polygon({{x + 12, y}, {r, y}, {r, b - 12}, {r - 12, b}, {x, b}, {x, y + 12}}, color);
}

void stars(const Stage& s) {
    s.tiles(PrVec2{960, 640}, [&](PrVec2 corner) {
        for (uint32_t i = 0; i < 110; ++i) {
            PrVec2 p = corner + PrVec2{float((i * 313 + 47) % 960), float((i * 157 + 23) % 640)};
            float alpha = .12f + .2f * (1 + prF32Sin(s.t * .6f + float(i)));
            s.capsule(p, p, i % 9 == 0 ? 1.2f : .55f, fade(kCyan, alpha));
        }
    });
}

void reactor(const Stage& s) {
    const float t = s.t;
    s.fill(PrColor{.018f, .02f, .052f, 1});
    stars(s);
    const PrVec2 center{330, 358};
    s.glow(center, 280, fade(kViolet, .45f));
    s.glow(center, 190, fade(kCyan, .65f));
    s.label("NOVA / ENGINE LAB", 40, 45, 12, kCyan);
    s.label("ION DRIVE", 40, 95, 42, kWhite);
    s.label("EXPERIMENTAL PROPULSION // MK.07", 42, 121, 10, fade(kCyan, .5f));
    for (int i = 0; i < 80; ++i) {
        float angle = float(i) * kPi / 40;
        s.capsule(orbit(center, 225, angle), orbit(center, i % 5 == 0 ? 237.0f : 230.0f, angle),
            i % 5 == 0 ? 1.2f : .5f, fade(kCyan, i % 5 == 0 ? .8f : .22f));
    }
    s.ring(center, 210, 1, 0, 2 * kPi, fade(kViolet, .45f));
    s.ring(center, 195, 2, -t * .15f, 1.4f * kPi, kCyan);
    s.ring(center, 184, 1, t * .3f, .8f * kPi, kHot);
    for (int i = 0; i < 12; ++i) {
        float angle = float(i) * kPi / 6 + t * .24f;
        s.ring(center, 160, 12, angle, kPi / 9, fade(i % 3 == 0 ? kHot : kCyan, .65f));
        s.ring(center, 144, 2, -angle, kPi / 8, fade(kViolet, .8f));
    }
    for (int i = 0; i < 6; ++i) {
        float a = float(i) * kPi / 3 - t * .19f;
        PrVec2 tip = orbit(center, 120, a);
        s.polygon({
            orbit(center, 42, a - .22f), orbit(center, 94, a - .18f), tip, orbit(center, 102, a + .15f),
            orbit(center, 53, a + .36f),
        }, fade(i % 2 ? kCyan : kViolet, .18f));
        s.beam(orbit(center, 50, a), tip, .85f, fade(kCyan, .8f));
    }
    s.glow(center, 68 + 5 * prF32Sin(t * 3), kCyan);
    s.capsule(center, center, 23, kCyan);
    s.capsule(center, center, 12, kWhite);
    for (int i = 0; i < 54; ++i) {
        float phase = prF32Fmod(float(i) * .618034f + t * .15f, 1.0f);
        float a = float(i) * 2.399963f + t * .12f;
        float r = 58 + phase * 196;
        s.beam(orbit(center, r - 5 - phase * 9, a), orbit(center, r, a), .65f,
            fade(i % 3 == 0 ? kHot : kCyan, (1 - phase) * .75f));
    }
    cutPanel(s, PrRect{632, 171, 918, 595}, PrColor{.035f, .045f, .095f, .94f});
    s.beam(PrVec2{633, 193}, PrVec2{633, 303}, 1, kViolet);
    s.label("CORE OUTPUT", 658, 211, 12, fade(kCyan, .65f));
    s.label(format("{:05.1f}", 84.0 + prF64Sin(double(t) * 1.4) * 1.7), 653, 281, 57, kWhite);
    s.label("TERAWATTS", 658, 309, 11, kCyan);
    for (int i = 0; i < 34; ++i) {
        float h = 14 + 22 * (1 + prF32Sin(float(i) * .6f - t * 2)) + 8 * prF32Cos(float(i) * 1.3f + t);
        s.rect(658 + float(i) * 6.5f, 411 - h, 3.5f, h, fade(i > 27 ? kHot : kCyan, .75f), 1);
    }
    s.label("CONTAINMENT", 658, 452, 11, fade(kCyan, .65f));
    s.label("FIELD STABLE", 658, 480, 18, kWhite);
    cutPanel(s, PrRect{655, 527, 891, 569}, fade(kViolet, .35f));
    s.label("OVERDRIVE READY", 675, 553, 13, kWhite);
    s.label("REACTOR 07", 58, 625, 11, kCyan);
    s.label("STATUS / SYNCHRONIZED", 254, 625, 11, fade(kCyan, .55f));
    s.label("CHARGE / 84%", 654, 625, 11, kViolet);
}

void combat(const Stage& s) {
    const float t = s.t;
    const PrColor amber{1, .63f, .17f, 1};
    const PrColor acid{.76f, .91f, .28f, 1};
    const PrColor ember{1, .27f, .075f, 1};
    s.fill(PrColor{.055f, .065f, .045f, 1});
    s.across(960, [&](float left) {
        for (uint32_t i = 0; i < 72; ++i) {
            float x = left + float((i * 313 + 47) % 960);
            float y = s.view.top + prF32Fmod(float(i * 157) + t * 6, 410 - s.view.top);
            s.capsule(PrVec2{x, y}, PrVec2{x - 2, y + 4}, .55f, fade(amber, .14f + .1f * prF32Sin(t + float(i))));
        }
    });
    s.glow(PrVec2{490, 340}, 330, fade(amber, .7f));
    s.across(1170, [&](float left) {
        for (int i = 0; i < 15; ++i) {
            float x = left + float(i) * 78 - 50;
            float h = 25 + float((i * 43) % 96);
            s.polygon({{x, 409}, {x + 30, 409 - h}, {x + 51, 415 - h}, {x + 95, 409}}, PrColor{.12f, .13f, .075f, 1});
        }
    });
    s.beam(PrVec2{s.view.left, 411}, PrVec2{s.view.right, 411}, .8f, fade(PrColor{.94f, .50f, .14f, 1}, .8f));
    // The grid's lines run from the horizon to the view's bottom, spreading as they did to the scene's.
    float reach = (s.view.bottom - 412) / 308;
    for (int i = -16; i <= 16; ++i)
        s.capsule(PrVec2{480 + float(i) * 26, 412}, PrVec2{480 + float(i) * (26 + 154 * reach), s.view.bottom}, .55f,
            fade(amber, .22f));
    for (int i = 0; i < 12; ++i) {
        float phase = prF32Fmod(float(i) / 12 + t * .055f, 1.0f);
        float y = 414 + phase * phase * 300 * reach;
        s.capsule(PrVec2{s.view.left, y}, PrVec2{s.view.right, y}, .6f, fade(amber, .12f + phase * .24f));
    }
    s.label("GHOST RUNNER", 40, 64, 31, kWhite);
    s.label("SECTOR 09  /  INTERCEPT", 42, 93, 11, acid);
    s.label("LIVE FEED", 624, 50, 11, ember);
    s.capsule(PrVec2{607, 46}, PrVec2{607, 46}, 3 + prF32Sin(t * 3), ember);
    PrVec2 ship{478 + 26 * prF32Sin(t * .9f), 306 + 16 * prF32Cos(t * 1.2f)};
    s.glow(PrVec2{ship.x, ship.y + 47}, 80, fade(ember, .8f));
    s.beam(PrVec2{ship.x - 24, ship.y + 38}, PrVec2{ship.x - 29, ship.y + 88 + 10 * prF32Sin(t * 12)}, 3, ember);
    s.beam(PrVec2{ship.x + 24, ship.y + 38}, PrVec2{ship.x + 29, ship.y + 88 + 10 * prF32Cos(t * 12)}, 3, ember);
    s.polygon({
        {ship.x, ship.y - 85},
        {ship.x + 91, ship.y + 49},
        {ship.x + 34, ship.y + 24},
        {ship.x, ship.y + 45},
        {ship.x - 34, ship.y + 24},
        {ship.x - 91, ship.y + 49},
    }, PrColor{.16f, .18f, .105f, 1});
    s.beam(PrVec2{ship.x - 91, ship.y + 49}, PrVec2{ship.x, ship.y - 85}, 1, acid);
    s.beam(PrVec2{ship.x, ship.y - 85}, PrVec2{ship.x + 91, ship.y + 49}, 1, acid);
    s.beam(PrVec2{ship.x, ship.y - 61}, PrVec2{ship.x, ship.y + 29}, 1, fade(amber, .8f));
    s.polygon({{ship.x, ship.y - 52}, {ship.x + 17, ship.y - 10}, {ship.x, ship.y + 6}, {ship.x - 17, ship.y - 10}},
        fade(acid, .55f));
    PrVec2 aim = s.at;
    for (int i = 0; i < 4; ++i) {
        float a = float(i) * kPi / 2 + kPi / 4;
        s.ring(aim, 110, 1.5f, a - .16f, .32f, fade(acid, .75f));
        s.beam(orbit(aim, 124, a), orbit(aim, 136, a), 1, acid);
    }
    s.capsule(PrVec2{aim.x - 8, aim.y}, PrVec2{aim.x + 8, aim.y}, .7f, kWhite);
    s.capsule(PrVec2{aim.x, aim.y - 8}, PrVec2{aim.x, aim.y + 8}, .7f, kWhite);
    s.label("TRACKING", aim.x + 123, aim.y - 71, 10, acid);
    cutPanel(s, PrRect{38, 501, 282, 650}, PrColor{.06f, .075f, .045f, .93f});
    s.label("PLASMA LANCE", 58, 532, 12, acid);
    s.label("024", 55, 590, 47, kWhite);
    s.label("/ 120", 158, 587, 19, fade(acid, .55f));
    for (int i = 0; i < 18; ++i)
        s.rect(59 + float(i) * 10, 615, 6, 12, i < 15 ? acid : fade(acid, .15f));
    const PrVec2 radar{828, 556};
    s.capsule(radar, radar, 92, PrColor{.07f, .095f, .04f, .95f});
    for (int i = 1; i < 4; ++i)
        s.ring(radar, float(i) * 27, 1, 0, 2 * kPi, fade(acid, .2f));
    s.capsule(PrVec2{radar.x - 82, radar.y}, PrVec2{radar.x + 82, radar.y}, .5f, fade(acid, .2f));
    s.capsule(PrVec2{radar.x, radar.y - 82}, PrVec2{radar.x, radar.y + 82}, .5f, fade(acid, .2f));
    for (int i = 0; i < 16; ++i)
        s.ring(radar, 81, 80, t - float(i) * .018f, .018f, fade(acid, .12f * (1 - float(i) / 16)));
    s.beam(radar, orbit(radar, 81, t), .8f, acid);
    for (int i = 0; i < 5; ++i) {
        PrVec2 dot = orbit(radar, 24 + float(i) * 11, float(i) * 2.4f);
        s.capsule(dot, dot, 2.5f, i % 2 ? ember : acid);
    }
    const char* keys[] = {"Q", "E", "R"};
    const char* names[] = {"DASH", "PULSE", "CLOAK"};
    for (int i = 0; i < 3; ++i) {
        PrVec2 c{365 + float(i) * 116, 592};
        float progress = prF32Fmod(t * .08f + float(i) * .27f, 1.0f);
        s.capsule(c, c, 32, PrColor{.11f, .13f, .065f, .9f});
        s.ring(c, 35, 3, -kPi / 2, 2 * kPi * progress, i == 1 ? ember : acid);
        s.label(keys[i], c.x - 8, c.y + 8, 22, kWhite);
        s.label(names[i], c.x - 20, 645, 10, fade(acid, .6f));
    }
}

void jelly(const Stage& s) {
    const float t = s.t;
    const PrColor plum{.24f, .17f, .30f, 1};
    const PrColor cream{1, .93f, .76f, 1};
    s.fill(cream);
    s.capsule(PrVec2{624, 159}, PrVec2{624, 159}, 86, PrColor{1, .71f, .36f, 1});
    for (int i = 0; i < 7; ++i) {
        float x = s.wrap(float(i) * 169 + t * 8, 100);
        float y = 160 + float(i % 3) * 52;
        s.capsule(PrVec2{x, y}, PrVec2{x + 54, y}, 18, PrColor{1, .98f, .90f, .65f});
    }
    s.label("JELLY", 42, 79, 58, plum);
    s.label("JAM!", 39, 148, 78, plum);
    s.label("A LITTLE SHAPE-SHIFTING MISCHIEF", 46, 179, 11, fade(plum, .7f));
    for (int layer = 0; layer < 2; ++layer) {
        Array<PrVec2> hill({{s.view.left, s.view.bottom}});
        float x = s.view.left;
        for (; x < s.view.right + 24; x += 24)
            hill.push({
                x, layer == 0 ? 460 + 25 * prF32Sin(x * .009f + t * .2f) : 556 + 20 * prF32Sin(x * .012f - t * .12f),
            });
        hill.push({x - 24, s.view.bottom});
        s.polygon(hill, layer == 0 ? PrColor{.73f, .82f, .58f, 1} : PrColor{.50f, .69f, .49f, 1});
    }
    const PrVec2 positions[] = {{387, 332}, {684, 328}, {190, 449}, {463, 468}, {761, 454}};
    const PrColor colors[] = {
        {.67f, .73f, .98f, 1},
        {.91f, .53f, .67f, 1},
        {1, .62f, .31f, 1},
        {.48f, .76f, .70f, 1},
        {.83f, .62f, .90f, 1},
    };
    PrVec2 pointer = s.at;
    // Held, the jellies stop bouncing and squash flat on the ground.
    float held = s.hold * s.hold * (3 - 2 * s.hold);
    for (int i = 0; i < 5; ++i) {
        float phase = t * 2 + float(i) * 1.7f;
        float size = i < 2 ? 92.0f : 143.0f;
        float bounce = max(0.f, prF32Sin(phase)) * 19 * (1 - held);
        float squash = 1 + .08f * prF32Cos(phase * 2) * (1 - held) + .45f * held;
        float sink = size * .5f * (1 - 1 / squash);
        PrVec2 p = positions[i];
        prDrawListEllipse(s.list, PrVec2{p.x, p.y + size * .48f}, PrVec2{size * .43f * (1 + .3f * held), 8}, 0,
            {.color = fade(plum, .10f)});
        s.push(turn(.09f * prF32Sin(phase) * (1 - held), PrVec2{p.x, p.y - bounce + sink}, squash, 1 / squash));
        PrCornerRect blob{PrRect{-size * .5f, -size * .5f, size * .5f, size * .5f}, {}};
        for (uint32_t corner = 0; corner < 4; ++corner)
            blob.corners[corner] = PrCorner{
                PrVec2{size * .42f, size * .42f},
                .6f + 1.6f * prF32Sin(t * .6f + float(i) * 1.9f + float(corner) * .5f),
            };
        prDrawListCornerRect(s.list, blob, {.color = colors[i]});
        float eye = size * .1f;
        float separation = size * .17f;
        float gaze = clamp((pointer.x - p.x) * .012f, -3.0f, 3.0f);
        bool blink = prF32Sin(t * .8f + float(i) * .9f) > .993f;
        const float eyes[] = {-separation, separation};
        for (float x : eyes) {
            if (blink) {
                s.capsule(PrVec2{x - eye * .6f, -5}, PrVec2{x + eye * .6f, -5}, 2, plum);
            } else {
                s.capsule(PrVec2{x, -7}, PrVec2{x, -3}, eye, cream);
                s.capsule(PrVec2{x + gaze, -5}, PrVec2{x + gaze, -3}, eye * .43f, plum);
            }
        }
        s.ring(PrVec2{0, size * .07f}, size * .09f, 2.5f, 0, kPi, plum);
        s.capsule(PrVec2{-size * .31f, 9}, PrVec2{-size * .25f, 9}, size * .034f, fade(kCoral, .4f));
        s.capsule(PrVec2{size * .25f, 9}, PrVec2{size * .31f, 9}, size * .034f, fade(kCoral, .4f));
        s.pop();
    }
    for (int i = 0; i < 14; ++i) {
        float x = 22 + float(i) * 70;
        float y = 570 + 13 * prF32Sin(float(i) * 1.8f);
        float sway = 3 * prF32Sin(t + float(i));
        s.capsule(PrVec2{x, y + 17}, PrVec2{x + sway, y}, 1.5f, fade(plum, .35f));
        for (int petal = 0; petal < 5; ++petal) {
            PrVec2 point = orbit(PrVec2{x + sway, y}, 5, float(petal) * 2 * kPi / 5);
            s.capsule(point, point, 4, cream);
        }
        s.capsule(PrVec2{x + sway, y}, PrVec2{x + sway, y}, 2.5f, kCoral);
    }
    // The button sinks while it is held.
    float press = 3 * held;
    s.rect(348, 612, 264, 43, fade(plum, .35f), 21);
    s.rect(348, 609 + press, 264, 43, plum, 21);
    s.label("HOLD TO SQUISH", 383, 637 + press, 16, cream);
}

// Returns a brush of one palette, from blue to orange, mixed in `space`.
PrDrawBrush chromaBrush(PrColorSpace space, const PrDrawColorStop* stops, uint32_t count, float from, float to) {
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{from, 0}, PrVec2{to, 0}, stops, count);
    brush.interpolationSpace = space;
    brush.hueMethod = PrColorHueMethod_Shorter;
    return brush;
}

void chroma(const Stage& s) {
    const PrColor sheet{.93f, .94f, .96f, 1};
    const PrColor graphite{.13f, .13f, .20f, 1};
    const PrColorSpace spaces[] = {
        PrColorSpace_Srgb,  PrColorSpace_SrgbLinear, PrColorSpace_Lab,
        PrColorSpace_Oklab, PrColorSpace_Hsl,        PrColorSpace_Oklch,
    };
    const char* names[] = {"sRGB", "LINEAR", "LAB", "OKLAB", "HSL", "OKLCH"};
    const PrDrawColorStop palette[] = {
        {.offset = 0, .color = PrColor{.06f, .25f, .92f, 1}},
        {.offset = 1, .color = PrColor{1, .28f, .035f, 1}},
    };
    const PrDrawColorStop sheen[] = {
        {.offset = 0, .color = PrColor{1, 1, 1, 0}},
        {.offset = .48f, .color = PrColor{1, 1, 1, .7f}},
        {.offset = 1, .color = PrColor{1, 1, 1, 0}},
    };
    // The space that the silk shows changes every few seconds.
    uint32_t shown = uint32_t(s.t / 4) % 6;
    s.fill(sheet);
    s.label("CHROMA", 42, 112, 76, graphite);
    s.label("COLOR, IN MOTION", 48, 146, 13, graphite);
    s.label("A STUDY IN LIGHT & SILK", 48, 50, 10, kMuted);
    float phase = .22f * s.t;
    auto curve = [&](float u, float strand) {
        float fold = u * 2.1f * kPi + phase;
        float spread = 36 + 80 * (.5f + .5f * prF32Sin(u * 2 * kPi - phase));
        return PrVec2{
            s.view.left - 60 + (s.view.right - s.view.left + 120) * u + strand * 25 * prF32Cos(fold),
            360 + 108 * prF32Sin(fold) + strand * spread * prF32Cos(fold * .65f + phase),
        };
    };
    prDrawListPushClipRect(s.list, PrRect{s.view.left, 168, s.view.right, 537}, PrDrawEdges_Antialiased);
    PrDrawStroke stroke = strokeOf(3.8f, PrPathJoin_Round, PrPathCap_Round);
    PrVec2 points[129];
    for (int strand = 0; strand < 45; ++strand) {
        float across = float(strand) / 22 - 1;
        for (int i = 0; i < 129; ++i)
            points[i] = curve(float(i) / 128, across);
        // Each strand's palette, darkened by the light that falls on it.
        float lighting = .62f + .38f * prF32Pow(.5f + .5f * prF32Sin(across * 3.2f + phase), 2.f);
        PrDrawColorStop lit[2];
        for (uint32_t k = 0; k < 2; ++k) {
            PrColor c = palette[k].color;
            lit[k] = PrDrawColorStop{
                .offset = palette[k].offset,
                .color = PrColor{c.r * lighting, c.g * lighting, c.b * lighting, c.a},
            };
        }
        PrDrawBrush silk = chromaBrush(spaces[shown], lit, 2, s.view.left + 30, s.view.right - 30);
        prDrawListPolylineContour(s.list, points, 129, PrDrawContour_Open, PrPathFillRule_NonZero,
            {.brush = &silk, .stroke = &stroke});
    }
    stroke.shape.width = .7f;
    PrDrawBrush highlight = chromaBrush(spaces[shown], sheen, 3, s.view.left + 30, s.view.right - 30);
    for (int strand = 0; strand < 6; ++strand) {
        for (int i = 0; i < 129; ++i)
            points[i] = curve(float(i) / 128, -.95f + float(strand) * .38f);
        prDrawListPolylineContour(s.list, points, 129, PrDrawContour_Open, PrPathFillRule_NonZero,
            {.brush = &highlight, .stroke = &stroke});
    }
    prDrawListPopClip(s.list);
    s.label("ONE PALETTE. SIX JOURNEYS.", 48, 567, 11, graphite);
    s.label("EACH IN ITS OWN SPACE", 665, 567, 11, kMuted);
    stroke.shape.width = 12;
    for (uint32_t i = 0; i < 6; ++i) {
        float x = 48 + float(i) * 147;
        PrDrawBrush preview = chromaBrush(spaces[i], palette, 2, x, x + 128);
        prDrawListLine(s.list, PrVec2{x, 595}, PrVec2{x + 128, 595}, {.brush = &preview, .stroke = &stroke});
        s.label(names[i], x, 625, 12, graphite);
        if (i == shown)
            s.rect(x, 641, 128, 2, graphite);
    }
}

// Draws a border of one style and color around the area, with the corners' radius and shape.
void postcardFrame(const Stage& s, PrRect area, PrDrawBorderStyle style, PrColor color, float width,
        float shape = 1, float radius = 12, uint32_t periods = 0) {
    PrDrawBorder border{};
    border.shape.rect = area;
    for (PrCorner& corner : border.shape.corners)
        corner = PrCorner{PrVec2{radius, radius}, shape};
    for (PrDrawBorderSide& side : border.sides)
        side = PrDrawBorderSide{width, style, color, nullptr, periods};
    prDrawListBorder(s.list, &border, {});
}

void airmail(const Stage& s) {
    const float t = s.t;
    const PrColor cream{.99f, .965f, .87f, 1};
    const PrColor blue{.10f, .23f, .51f, 1};
    const PrColor red{.80f, .20f, .17f, 1};
    const PrColor green{.18f, .39f, .32f, 1};
    const PrColor sky{.54f, .76f, .79f, 1};
    const PrDrawBorderStyle postal[] = {
        PrDrawBorderStyle_Dashed, PrDrawBorderStyle_Dotted, PrDrawBorderStyle_Solid,
        PrDrawBorderStyle_Double,
    };
    const char* postalName[] = {"DASHED", "DOTTED", "SOLID", "DOUBLE"};
    uint32_t style = uint32_t(t / 5) % 4;
    s.fill(PrColor{.89f, .87f, .79f, 1});
    // The ruled lines: slanting down from under the heading, and across, falling slightly to the left.
    float lean = 110.0f / 550 * (s.view.bottom - 170);
    for (float x = -100 - 47 * prF32Ceil((lean - s.view.left - 100) / 47); x < s.view.right; x += 47)
        s.capsule(PrVec2{x, 170}, PrVec2{x + lean, s.view.bottom}, .6f, fade(blue, .04f));
    for (float y = 175; y < s.view.bottom + 30; y += 27)
        s.capsule(PrVec2{s.view.left, y - 30 * s.view.left / 960}, PrVec2{s.view.right, y - 30 * s.view.right / 960},
            .6f, fade(blue, .035f));
    s.label("THE POST OFFICE / COLLECTED JOURNEYS", 48, 48, 10, blue);
    s.label("AIRMAIL", 42, 115, 76, blue);
    s.label("Small worlds. Long journeys.", 48, 147, 15, red);

    s.push(turn(-.19f + .014f * prF32Sin(t * .3f), PrVec2{202, 364 + 5 * prF32Sin(t * .45f)}));
    const PrCornerRect cut{PrRect{-116, -143, 116, 143}, {{{15, 15}, 0}, {{15, 15}, 0}, {{15, 15}, 0}, {{15, 15}, 0}}};
    prDrawListCornerRect(s.list, cut, {.color = cream});
    postcardFrame(s, PrRect{-116, -143, 116, 143}, PrDrawBorderStyle_Double, green, 12, 0, 15);
    s.rect(-93, -117, 186, 213, green);
    s.capsule(PrVec2{22, -59}, PrVec2{22, -59}, 35, PrColor{.95f, .72f, .37f, 1});
    s.polygon({{-93, 50}, {-42, -44}, {14, 50}}, PrColor{.11f, .28f, .27f, 1});
    s.polygon({{-26, 59}, {44, -21}, {93, 42}, {93, 96}, {-26, 96}}, PrColor{.12f, .33f, .29f, 1});
    for (int i = 0; i < 5; ++i) {
        float y = 47 + float(i) * 10;
        s.capsule(PrVec2{-84 + 7 * prF32Sin(t + float(i)), y}, PrVec2{81, y - 3}, .7f, fade(cream, .4f));
    }
    s.label("NORTH COAST", -88, 120, 16, green);
    s.pop();

    s.push(turn(.17f + .016f * prF32Sin(t * .35f), PrVec2{755, 354 + 6 * prF32Sin(t * .5f + 2)}));
    const PrCornerRect smooth{
        PrRect{-113, -132, 113, 132},
        {{{22, 22}, 2}, {{22, 22}, 2}, {{22, 22}, 2}, {{22, 22}, 2}},
    };
    prDrawListCornerRect(s.list, smooth, {.color = PrColor{.89f, .56f, .39f, 1}});
    postcardFrame(s, PrRect{-113, -132, 113, 132}, PrDrawBorderStyle_Groove, PrColor{.94f, .72f, .49f, 1}, 13, 2, 22);
    s.rect(-88, -106, 176, 181, cream);
    postcardFrame(s, PrRect{-88, -106, 88, 75}, PrDrawBorderStyle_Inset, PrColor{.83f, .63f, .43f, 1}, 4, 0, 0);
    s.capsule(PrVec2{0, 62}, PrVec2{0, -4}, 3, green);
    for (int petal = 0; petal < 8; ++petal) {
        float angle = float(petal) * kPi / 4 + .1f * prF32Sin(t * .7f);
        s.capsule(orbit(PrVec2{0, -30}, 12, angle), orbit(PrVec2{0, -30}, 38, angle), 12, red);
    }
    s.capsule(PrVec2{0, -30}, PrVec2{0, -30}, 15, PrColor{.96f, .70f, .27f, 1});
    s.label("SUN ROOM", -78, 108, 19, cream);
    s.pop();

    float cardWidth = 432 + 64 * prF32Sin(t * .65f);
    float cardHeight = 286 + 38 * prF32Sin(t * .8f + 1);
    s.push(turn(-.035f, PrVec2{477, 398}));
    PrDrawBorder border{};
    border.shape.rect = PrRect{-cardWidth / 2, -cardHeight / 2, cardWidth / 2, cardHeight / 2};
    for (PrCorner& corner : border.shape.corners)
        corner = PrCorner{PrVec2{14, 14}, -.5f};
    prDrawListCornerRect(s.list, border.shape, {.color = cream});
    const uint32_t periods[] = {11, 7, 11, 7};
    for (uint32_t i = 0; i < 4; ++i)
        border.sides[i] = PrDrawBorderSide{
            10, postal[style], i % 2 ? blue : red, nullptr,
            periods[i] * (postal[style] == PrDrawBorderStyle_Dotted ? 2u : 1u),
        };
    prDrawListBorder(s.list, &border, {});
    float artwork = min(cardWidth / 432, cardHeight / 286);
    s.push(PrAffine2{artwork, 0, 0, artwork, 0, 0});
    s.rect(-192, -116, 228, 230, sky);
    postcardFrame(s, PrRect{-192, -116, 36, 114}, PrDrawBorderStyle_Solid, blue, 2, 0, 0);
    s.capsule(PrVec2{-30, -66}, PrVec2{-30, -66}, 33, PrColor{.96f, .62f, .27f, 1});
    s.polygon({{-190, 1}, {-121, -74}, {-37, 12}}, PrColor{.32f, .49f, .51f, 1});
    s.polygon({{-146, 13}, {-69, -34}, {34, 10}, {34, 114}, {-146, 114}}, PrColor{.20f, .39f, .45f, 1});
    s.rect(-190, 18, 224, 95, PrColor{.18f, .48f, .60f, 1});
    for (int i = 0; i < 8; ++i) {
        float x = -180 + float((i * 53) % 153) + 8 * prF32Sin(t * .8f + float(i));
        float y = 30 + float(i) * 10;
        s.capsule(PrVec2{x, y}, PrVec2{x + 32, y}, .8f, fade(cream, .55f));
    }
    float bird = 9 * prF32Sin(t);
    s.capsule(PrVec2{-161, -83}, PrVec2{-152, -78 + bird * .2f}, 1.3f, blue);
    s.capsule(PrVec2{-152, -78 + bird * .2f}, PrVec2{-143, -83}, 1.3f, blue);
    s.label("WISH YOU", 57, -69, 16, blue);
    s.label("WERE HERE.", 57, -43, 16, blue);
    for (int line = 0; line < 4; ++line)
        s.capsule(PrVec2{58, -11 + float(line) * 18}, PrVec2{181, -11 + float(line) * 18}, .55f, fade(blue, .35f));
    postcardFrame(s, PrRect{153, -116, 188, -77}, PrDrawBorderStyle_Dotted, blue, 3, 0, 0);
    s.label("P", 163, -89, 21, red);
    s.ring(PrVec2{99, 79}, 35, 1.5f, 0, 2 * kPi, fade(blue, .8f));
    s.ring(PrVec2{99, 79}, 29, .6f, 0, 2 * kPi, fade(blue, .8f));
    s.label("SEP", 87, 74, 11, blue);
    s.label("11", 89, 91, 15, blue);
    for (int line = 0; line < 4; ++line)
        s.capsule(PrVec2{123, 66 + float(line) * 8}, PrVec2{191, 60 + float(line) * 8}, .8f, fade(blue, .7f));
    s.pop();
    s.pop();

    s.rect(48, 596, 252, 55, PrColor{.94f, .75f, .44f, 1}, 8);
    postcardFrame(s, PrRect{48, 596, 300, 651}, PrDrawBorderStyle_Outset, PrColor{.91f, .66f, .32f, 1}, 5, 1, 8);
    s.label("FIRST CLASS", 67, 621, 13, blue);
    s.label("NO RETURN TICKET", 67, 640, 10, blue);
    s.label("A NEW EDGE EVERY FEW SECONDS", 630, 591, 10, blue);
    s.rect(630, 603, 281, 48, cream, 8);
    postcardFrame(s, PrRect{630, 603, 911, 651}, PrDrawBorderStyle_Ridge, blue, 3, 1, 8);
    s.label(format("EDGE / {}", postalName[style]), 650, 633, 15, blue);
}

// Draws a shadow of the rounded box, dropped or inner.
void radioShadow(const Stage& s, PrRect box, float radius, PrVec2 offset, float blur, float spread, PrColor color,
        bool inner = false) {
    PrDrawShadow shadow{};
    shadow.shape.rect = box;
    for (PrCorner& corner : shadow.shape.corners)
        corner = PrCorner{PrVec2{radius, radius}, 1};
    shadow.offset = offset;
    shadow.sigma = blur / 2;
    shadow.spread = spread;
    if (inner)
        prDrawListInnerShadow(s.list, &shadow, {.color = color});
    else
        prDrawListDropShadow(s.list, &shadow, {.color = color});
}

void sunroom(const Stage& s) {
    const float t = s.t;
    const PrColor clay{.73f, .27f, .16f, 1};
    const PrColor plum{.25f, .14f, .19f, 1};
    const PrColor cream{1, .89f, .67f, 1};
    const PrColor olive{.28f, .34f, .20f, 1};
    s.fill(PrColor{.91f, .70f, .53f, 1});
    // Broad shafts of afternoon light drift across the tabletop.
    s.push(PrAffine2{1, 0, -.48f, 1, 100 + 14 * prF32Sin(t * .2f), 0});
    float from = s.view.left + .48f * s.view.top - 300;
    for (float x = -240 + 245 * prF32Floor((from + 240) / 245); x < s.view.right + .48f * s.view.bottom; x += 245)
        s.rect(x, s.view.top, 160, s.view.bottom - s.view.top, PrColor{1, .94f, .72f, .22f});
    s.pop();
    s.label("SLOW DAYS / GOOD FREQUENCIES", 48, 53, 12, clay);
    s.label("Sunday stereo.", 45, 120, 54, plum);
    s.label("Leave a little room for doing nothing.", 48, 154, 14, plum);
    s.push(turn(-.035f + .012f * prF32Sin(t * .4f), PrVec2{477, 389 + 4 * prF32Sin(t * .7f)}));
    radioShadow(s, PrRect{-332, -163, 332, 173}, 44, PrVec2{16 + 6 * prF32Sin(t * .35f), 26}, 34, -3, fade(plum, .33f));
    radioShadow(s, PrRect{-332, -163, 332, 173}, 44, PrVec2{3, 7}, 8, 1, fade(plum, .22f));
    s.rect(-332, -163, 664, 336, clay, 44);
    s.rect(-326, -161, 652, 321, cream, 41);
    radioShadow(s, PrRect{-326, -161, 326, 160}, 41, PrVec2{-2, -4}, 9, 0, PrColor{1, 1, .92f, .65f}, true);
    s.rect(-152, -175, 12, 20, clay, 3);
    s.rect(140, -175, 12, 20, clay, 3);
    radioShadow(s, PrRect{-160, -196, 160, -171}, 10, PrVec2{2, 3}, 8, 0, fade(plum, .25f));
    s.rect(-160, -196, 320, 25, clay, 10);
    s.rect(-142, -189, 284, 8, plum, 4);
    s.rect(-290, -123, 282, 242, PrColor{.53f, .30f, .20f, 1}, 28);
    radioShadow(s, PrRect{-290, -123, -8, 119}, 28, PrVec2{6, 9}, 15, 2, fade(plum, .75f), true);
    prDrawListPushClipRect(s.list, PrRect{-278, -111, -20, 107}, PrDrawEdges_Antialiased);
    for (int row = 0; row < 18; ++row) {
        for (int column = 0; column < 22; ++column) {
            PrVec2 p{-274 + float(column) * 12 + (row % 2 ? 6.f : 0.f), -104 + float(row) * 12};
            float pulse = .82f + .18f * prF32Sin(t * 3 - float(row) * .18f - float(column) * .12f);
            s.capsule(p, p, 2.3f, fade(cream, pulse * .6f));
        }
    }
    prDrawListPopClip(s.list);
    s.label("SUNROOM", -249, 144, 13, clay);
    s.rect(30, -123, 258, 90, olive, 15);
    radioShadow(s, PrRect{30, -123, 288, -33}, 15, PrVec2{4, 7}, 12, 1, fade(plum, .85f), true);
    float tune = 90 + 150 * (.5f + .5f * prF32Sin(t * .27f));
    for (int i = 0; i < 25; ++i) {
        float x = 47 + float(i) * 9;
        s.capsule(PrVec2{x, -61}, PrVec2{x, i % 5 == 0 ? -78.f : -71.f}, .7f, fade(cream, .65f));
    }
    s.rect(tune, -104, 3, 53, PrColor{.99f, .45f, .20f, 1}, 1);
    s.label("FM  88   94   100   108", 47, -89, 11, cream);
    s.label("WARM SOUND. NO HURRY.", 39, -12, 11, clay);
    const char* keys[] = {"PLAY", "AM", "FM"};
    for (int i = 0; i < 3; ++i) {
        float x = 34 + float(i) * 87;
        float depression = 2 + 2 * prF32Sin(t * 1.8f - float(i) * 2);
        PrRect key{x, 12 + depression, x + 73, 50 + depression};
        radioShadow(s, key, 10, PrVec2{0, 6 - depression}, 7, 0, fade(plum, .30f));
        s.rect(key.left, key.top, 73, 38, i == 0 ? clay : PrColor{.92f, .76f, .51f, 1}, 10);
        radioShadow(s, key, 10, PrVec2{0, -2}, 4, 0, fade(plum, .20f), true);
        s.label(keys[i], x + 16, key.top + 25, 12, i == 0 ? cream : plum);
    }
    for (int i = 0; i < 2; ++i) {
        float x = 67 + float(i) * 160;
        radioShadow(s, PrRect{x - 28, 74, x + 28, 130}, 28, PrVec2{3, 5}, 8, 0, fade(plum, .35f));
        s.rect(x - 28, 74, 56, 56, clay, 28);
        s.rect(x - 25, 75, 50, 50, PrColor{.97f, .80f, .56f, 1}, 25);
        float a = -.8f + .6f * prF32Sin(t * .27f + float(i));
        s.capsule(PrVec2{x + prF32Sin(a) * 13, 100 - prF32Cos(a) * 13},
            PrVec2{x + prF32Sin(a) * 22, 100 - prF32Cos(a) * 22}, 2, clay);
        s.label(i == 0 ? "VOLUME" : "TUNING", x - 22, 148, 10, clay);
    }
    s.pop();
    s.label("ON AIR", 49, 626, 11, clay);
    s.capsule(PrVec2{119, 622}, PrVec2{119, 622}, 3, fade(clay, .55f + .45f * prF32Sin(t * 2)));
    s.label("Sunroom sessions", 49, 654, 22, plum);
    s.label("AN AFTERNOON ON REPEAT", 648, 650, 12, plum);
}

// ---------------------------------------------------------------------------------------------------------------------
// Vertical Japanese, set as JIS X 4051 sets it: down columns on the em grid, the columns from right to left.

// Returns a vertical style of Noto Sans JP at `weight`.
TextStyle verticalStyle(float size, float weight, PrColor color) {
    PrFontVariation wght{PR_FONT_TAG('w', 'g', 'h', 't'), weight};
    TextStyle style;
    style.font = fontVaried("NotoSansJP-VF.otf", &wght, 1);
    style.size = size;
    style.paint.color = color;
    style.axis = PrTextAxis_Vertical;
    style.language = "ja";
    return style;
}

// Returns the style of the readings beside a vertical style: half its size, in the font's ruby forms.
TextStyle rubyStyle(const TextStyle& base) {
    static const PrTextFeature forms{PR_FONT_TAG('r', 'u', 'b', 'y'), 1, 0, ~0u};
    TextStyle style = base;
    style.size = base.size / 2;
    style.features = &forms;
    style.featureCount = 1;
    return style;
}

// Moves each glyph `tracking` further along the run's axis than the one before it.
void track(ShapedText& shaped, float tracking, PrTextAxis axis) {
    for (uint32_t i = 0; i < shaped.glyphs.count(); ++i) {
        float by = float(i) * tracking;
        if (axis == PrTextAxis_Vertical)
            shaped.glyphs[i].position.y += by;
        else
            shaped.glyphs[i].position.x += by;
    }
}

// Draws the text down the column whose center line passes through `top`, from `top`, with `tracking` between its
// characters. Returns the advance down.
float upright(const Stage& s, const TextStyle& style, PrStringView text, PrVec2 top, float tracking = 0) {
    ShapedText shaped;
    float down = shapeText(style, text, top, shaped);
    track(shaped, tracking, PrTextAxis_Vertical);
    drawGlyphs(s.list, style, shaped);
    return down + float(max<uint32_t>(shaped.glyphs.count(), 1) - 1) * tracking;
}

// Sets two digits across in the em of a vertical column that starts at `top` (縦中横), in the font's half-width forms,
// which fill the em between them, and squeezed only if they would not.
void combined(const Stage& s, const TextStyle& column, const char* digits, PrVec2 top) {
    static const PrTextFeature halfWidth{PR_FONT_TAG('h', 'w', 'i', 'd'), 1, 0, ~0u};
    TextStyle across = column;
    across.axis = PrTextAxis_Horizontal;
    across.features = &halfWidth;
    across.featureCount = 1;
    float width = textAdvance(across, digits);
    float squeeze = min(1.0f, column.size / width);
    float baseline = top.y + column.size / 2 + prFontMetrics(column.font).capHeight * column.size / 2;
    s.push(PrAffine2{squeeze, 0, 0, 1, top.x - width * squeeze / 2, baseline});
    drawText(s.list, across, digits, PrVec2{0, 0});
    s.pop();
}

// Sets Latin text sideways down the column whose center line passes through `top`, turned a quarter clockwise as a
// vertical line turns it, with its capitals centered on the line. Returns the advance down.
float sideways(const Stage& s, const TextStyle& style, PrStringView text, PrVec2 top, float tracking = 0) {
    ShapedText shaped;
    float capHeight = prFontMetrics(style.font).capHeight * style.size;
    float down = shapeText(style, text, PrVec2{0, capHeight / 2}, shaped);
    track(shaped, tracking, PrTextAxis_Horizontal);
    s.push(turn(kPi / 2, top));
    drawGlyphs(s.list, style, shaped);
    s.pop();
    return down + float(max<uint32_t>(shaped.glyphs.count(), 1) - 1) * tracking;
}

// Sets a reading beside its base, which runs `length` down from `top`, on the center line through `top`. A reading
// shorter than its base spreads with half as much space at its ends as between its characters, and a longer one
// overhangs both ends equally.
void ruby(const Stage& s, const TextStyle& style, PrStringView reading, PrVec2 top, float length) {
    ShapedText shaped;
    float down = shapeText(style, reading, top, shaped);
    if (shaped.glyphs.isEmpty())
        return;
    float count = float(shaped.glyphs.count());
    float space = (length - down) / count;
    for (uint32_t i = 0; i < shaped.glyphs.count(); ++i)
        shaped.glyphs[i].position.y += space > 0 ? space * (float(i) + .5f) : space * count / 2;
    drawGlyphs(s.list, style, shaped);
}

// A character of a paragraph: its code point, and its bytes.
struct Character {
    uint32_t codePoint;
    String text;
};

// A reading over `count` characters from `first`.
struct Reading {
    uint32_t first;
    uint32_t count;
    String text;
};

// Returns true if the code points of `set`, which a zero ends, hold `c`.
bool inSet(const char32_t* set, uint32_t c) {
    for (; *set != 0; ++set) {
        if (uint32_t(*set) == c)
            return true;
    }
    return false;
}

bool isKanji(uint32_t c) {
    return (c >= 0x4e00 && c <= 0x9fff) || c == U'々';
}

// Characters that do not begin a column (行頭禁則): closing brackets, punctuation, small kana, the long vowel mark and the
// iteration marks.
bool beginsNoColumn(uint32_t c) {
    return inSet(U"、。，．・：；？！ー…‥」』）〕】〉》ゝゞ々ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ", c);
}

// Characters that do not end a column (行末禁則): opening brackets.
bool endsNoColumn(uint32_t c) {
    return inSet(U"「『（〔【〈《", c);
}

// Punctuation that may hang below the end of a full column (ぶら下げ).
bool hangs(uint32_t c) {
    return inSet(U"、。，．", c);
}

// Reads text that marks its readings as Aozora Bunko does: a reading in 《》 follows the run of kanji that it reads, or
// the characters after a ｜.
void readRuby(PrStringView text, Array<Character>& characters, Array<Reading>& readings) {
    const PrStringView closing = "》";
    uint32_t marked = UINT32_MAX;
    for (size_t at = 0; at < text.size;) {
        uint32_t c = 0;
        uint32_t length = prUtf8Decode(text, at, &c);
        if (c == U'｜') {
            marked = characters.count();
        } else if (c == U'《') {
            size_t close = prStringFind(prStringSlice(text, at, text.size - at), closing);
            uint32_t first = characters.count();
            while (marked == UINT32_MAX && first > 0 && isKanji(characters[first - 1].codePoint))
                --first;
            if (marked != UINT32_MAX)
                first = marked;
            readings.push(Reading{first, characters.count() - first, prStringSlice(text, at + length, close - length)});
            marked = UINT32_MAX;
            length = uint32_t(close + closing.size);
        } else {
            characters.push(Character{c, prStringSlice(text, at, length)});
        }
        at += length;
    }
}

// Sets a paragraph down columns `length` ems long, from the column whose center line passes through `top` leftward,
// `pitch` apart, with its readings beside their bases. The paragraph begins one em down (字下げ). A column breaks where
// neither its end nor the next column's start breaks a rule: punctuation that would begin the next column hangs below
// this one, and otherwise characters move to the next column, and the column spreads to its full length.
void paragraph(const Stage& s, const TextStyle& style, PrStringView text, PrVec2 top, uint32_t length, float pitch) {
    Array<Character> characters;
    Array<Reading> readings;
    readRuby(text, characters, readings);
    const TextStyle readingStyle = rubyStyle(style);
    const float em = style.size;
    auto splitsReading = [&](uint32_t at) {
        for (const Reading& r : readings)
            if (r.first < at && at < r.first + r.count)
                return true;
        return false;
    };
    PrVec2 at = top;
    for (uint32_t start = 0; start < characters.count(); at.x -= pitch) {
        uint32_t room = start == 0 ? length - 1 : length;
        float y = start == 0 ? at.y + em : at.y;
        uint32_t end = min(start + room, characters.count());
        uint32_t hanging = 0;
        if (end < characters.count() && hangs(characters[end].codePoint)) {
            hanging = 1;
        } else {
            while (end < characters.count() && end > start + 1 &&
                    (beginsNoColumn(characters[end].codePoint) || endsNoColumn(characters[end - 1].codePoint) ||
                        splitsReading(end)))
                --end;
        }
        uint32_t set = end - start;
        end += hanging;
        String column;
        for (uint32_t i = start; i < end; ++i)
            column.append(characters[i].text);
        ShapedText shaped;
        shapeText(style, column, PrVec2{at.x, y}, shaped);
        float spread = 0;
        if (end < characters.count() && set < room && set > 1 && shaped.glyphs.count() == end - start)
            spread = float(room - set) * em / float(set - 1);
        track(shaped, spread, PrTextAxis_Vertical);
        drawGlyphs(s.list, style, shaped);
        for (const Reading& r : readings) {
            if (r.first < start || r.first + r.count > end)
                continue;
            float from = y + float(r.first - start) * (em + spread);
            ruby(s, readingStyle, r.text, PrVec2{at.x + em / 2 + readingStyle.size / 2 + .5f, from},
                float(r.count) * em + float(r.count - 1) * spread);
        }
        start = end;
    }
}

// Draws a seal (落款) pressed in `ink`, its characters cut out of the ink in `ground`, with the worn edge of a stone
// seal.
void seal(const Stage& s, PrVec2 center, float size, PrStringView text, PrColor ink, PrColor ground) {
    s.push(turn(-.04f, center));
    Array<PrVec2> edge;
    float h = size / 2;
    for (int side = 0; side < 4; ++side) {
        for (int k = 0; k < 9; ++k) {
            float along = -h + size * float(k) / 9;
            float wear = .7f * prF32Sin(float(side * 9 + k) * 12.9898f) * prF32Sin(float(k) * 4.1f + 1);
            PrVec2 p;
            if (side == 0)
                p = PrVec2{along, -h + wear};
            else if (side == 1)
                p = PrVec2{h - wear, along};
            else if (side == 2)
                p = PrVec2{-along, h - wear};
            else
                p = PrVec2{-h + wear, -along};
            edge.push(p);
        }
    }
    s.polygon(edge, ink);
    PrDrawStroke frame = strokeOf(1.1f);
    float in = size * .1f;
    prDrawListRect(s.list, PrRect{-h + in, -h + in, h - in, h - in}, {.color = fade(ground, .9f), .stroke = &frame});
    TextStyle carved = verticalStyle(size * .34f, 700, ground);
    upright(s, carved, text, PrVec2{0, -carved.size - size * .02f}, size * .04f);
    s.pop();
}

// Draws a maple leaf of `radius` at the origin, its seven lobes narrow and deep, and its stem down.
void mapleLeaf(const Stage& s, float radius, PrColor color) {
    Array<PrVec2> points({{0, radius * .2f}});
    const float lobes[] = {.45f, .78f, .94f, 1, .94f, .78f, .45f};
    for (int j = 0; j < 7; ++j) {
        float angle = -kPi / 2 + float(j - 3) * .7f;
        float reach = radius * lobes[j];
        points.push(orbit(PrVec2{0, 0}, reach * .62f, angle - .26f));
        points.push(orbit(PrVec2{0, 0}, reach, angle));
        points.push(orbit(PrVec2{0, 0}, reach * .62f, angle + .26f));
        if (j < 6)
            points.push(orbit(PrVec2{0, 0}, radius * .4f, angle + .35f));
    }
    s.polygon(points, color);
    s.capsule(PrVec2{0, 0}, PrVec2{0, radius * .9f}, radius * .06f, color);
}

// The height of each range of hills above the horizon at `x`: the far peak, the middle hills and the near trees.
float hills(int range, float x) {
    if (range == 0) {
        float peak = 205 * prF32Exp(-prF32Abs(x - 520) / 118);
        return min(peak, 190.0f) + 18 + 6 * prF32Sin(x * .02f);
    }
    if (range == 1)
        return 34 + 22 * prF32Sin(x * .011f + 1) + 12 * prF32Sin(x * .027f + .4f) +
            70 * prF32Exp(-(x - 170) * (x - 170) / 9000) + 44 * prF32Exp(-(x - 790) * (x - 790) / 16000);
    return 12 + 6 * prF32Sin(x * .05f) + 4 * prF32Sin(x * .13f + 2) + 3 * prF32Abs(prF32Sin(x * .31f));
}

// Draws a five-storied pagoda's silhouette, standing at `base`.
void pagoda(const Stage& s, PrVec2 base, PrColor color) {
    float y = base.y;
    for (int tier = 0; tier < 5; ++tier) {
        float body = 13 - float(tier) * 1.6f;
        float eave = 27 - float(tier) * 3.2f;
        s.rect(base.x - body / 2, y - 9, body, 9, color);
        y -= 9;
        s.polygon({
            {base.x - eave, y - 1.5f}, {base.x - eave + 4, y + .5f}, {base.x + eave - 4, y + .5f},
            {base.x + eave, y - 1.5f}, {base.x + eave * .55f, y - 4.5f}, {base.x - eave * .55f, y - 4.5f},
        }, color);
        y -= 4.5f;
    }
    s.capsule(PrVec2{base.x, y}, PrVec2{base.x, y - 24}, .9f, color);
    for (int ring = 0; ring < 5; ++ring)
        s.capsule(PrVec2{base.x - 2.2f, y - 6 - float(ring) * 3}, PrVec2{base.x + 2.2f, y - 6 - float(ring) * 3}, .7f, color);
}

// Draws a stone lantern's silhouette standing at `base`, its fire box lit.
void lantern(const Stage& s, PrVec2 base, PrColor stone, float flicker) {
    const PrColor fire{1, .74f, .42f, 1};
    s.glow(PrVec2{base.x, base.y - 70}, 90, fade(fire, .30f * flicker));
    s.rect(base.x - 20, base.y - 9, 40, 9, stone, 2);
    s.rect(base.x - 6, base.y - 46, 12, 38, stone);
    s.polygon({
        {base.x - 19, base.y - 46}, {base.x - 13, base.y - 54}, {base.x + 13, base.y - 54},
        {base.x + 19, base.y - 46},
    }, stone);
    s.rect(base.x - 13, base.y - 78, 26, 24, stone);
    s.rect(base.x - 6, base.y - 73, 12, 13, fade(fire, .55f + .35f * flicker), 1);
    s.polygon({
        {base.x - 35, base.y - 86}, {base.x - 26, base.y - 79}, {base.x + 26, base.y - 79},
        {base.x + 35, base.y - 86}, {base.x + 24, base.y - 87}, {base.x + 7, base.y - 98},
        {base.x - 7, base.y - 98}, {base.x - 24, base.y - 87},
    }, stone);
    s.capsule(PrVec2{base.x, base.y - 102}, PrVec2{base.x, base.y - 104}, 5, stone);
    s.capsule(PrVec2{base.x, base.y - 108}, PrVec2{base.x, base.y - 112}, 2.4f, stone);
}

void tsukimi(const Stage& s) {
    const float t = s.t;
    const float horizon = 430;
    const PrColor indigo{.06f, .07f, .14f, 1};       // 藍
    const PrColor wisteria{.27f, .23f, .38f, 1};     // 藤
    const PrColor toki{.90f, .62f, .55f, 1};         // 鴇
    const PrColor moonWhite{.96f, .95f, .88f, 1};    // 月白
    const PrColor kinari{.95f, .92f, .83f, 1};       // 生成
    const PrColor kincha{.82f, .67f, .44f, 1};       // 金茶
    const PrColor shu{.80f, .24f, .15f, 1};          // 朱
    const PrColor momiji{.78f, .18f, .12f, 1};       // 紅葉
    const PrColor ranges[] = {{.47f, .39f, .53f, 1}, {.29f, .24f, .37f, 1}, {.13f, .12f, .21f, 1}};

    // The sky at dusk: indigo overhead, warm where the sun has set.
    const PrDrawColorStop sky[] = {
        {.offset = 0, .color = indigo},
        {.offset = .45f, .color = PrColor{.14f, .13f, .26f, 1}},
        {.offset = .78f, .color = wisteria},
        {.offset = 1, .color = toki},
    };
    PrDrawBrush skyBrush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{0, horizon}, sky, 4);
    prDrawListRect(s.list, PrRect{s.view.left, s.view.top, s.view.right, horizon}, {.brush = &skyBrush});
    s.across(960, [&](float left) {
        for (int i = 0; i < 46; ++i) {
            PrVec2 p{left + float((i * 211 + 31) % 960), s.view.top + float((i * 97 + 13) % 230)};
            float alpha = (.18f + .14f * prF32Sin(t * .5f + float(i) * 2.3f)) * (1 - max(p.y, 0.0f) / 260);
            s.capsule(p, p, i % 7 == 0 ? .9f : .5f, fade(moonWhite, alpha));
        }
    });

    // The moon, with its seas, and thin cloud passing over it.
    const PrVec2 moon{300, 168};
    s.glow(moon, 250, fade(moonWhite, .45f));
    s.capsule(moon, moon, 54, moonWhite);
    prDrawListEllipse(s.list, PrVec2{moon.x - 15, moon.y - 12}, PrVec2{17, 13}, .4f, {.color = PrColor{.86f, .85f, .79f, .55f}});
    prDrawListEllipse(s.list, PrVec2{moon.x + 14, moon.y + 2}, PrVec2{11, 15}, -.3f, {.color = PrColor{.88f, .87f, .80f, .5f}});
    prDrawListEllipse(s.list, PrVec2{moon.x - 4, moon.y + 22}, PrVec2{9, 6}, 0, {.color = PrColor{.88f, .87f, .81f, .45f}});
    for (int i = 0; i < 3; ++i) {
        float x = s.wrap(t * (6 + float(i) * 2) + float(i) * 410, 170);
        s.push(PrAffine2{6, 0, 0, .3f, x, 120 + float(i) * 52});
        s.glow(PrVec2{0, 0}, 40, fade(PrColor{.42f, .37f, .52f, 1}, .45f));
        s.pop();
    }

    // The hills, farthest first, each with mist drifting along its foot; and their reflections in the still water.
    const PrDrawColorStop water[] = {
        {.offset = 0, .color = PrColor{.36f, .26f, .36f, 1}},
        {.offset = .35f, .color = PrColor{.11f, .10f, .19f, 1}},
        {.offset = 1, .color = PrColor{.03f, .035f, .07f, 1}},
    };
    PrDrawBrush waterBrush = prDrawBrushLinear(PrVec2{0, horizon}, PrVec2{0, kSceneHeight}, water, 3);
    prDrawListRect(s.list, PrRect{s.view.left, horizon, s.view.right, s.view.bottom}, {.brush = &waterBrush});
    for (int range = 0; range < 3; ++range) {
        Array<PrVec2> above({{s.view.right + 10, horizon}, {s.view.left - 10, horizon}});
        Array<PrVec2> below({{s.view.right + 10, horizon}, {s.view.left - 10, horizon}});
        for (float x = s.view.left - 10; x <= s.view.right + 10; x += 6) {
            above.push(PrVec2{x, horizon - hills(range, x)});
            below.push(PrVec2{x, horizon + hills(range, x) * .55f});
        }
        s.polygon(below, fade(ranges[range], .26f));
        s.polygon(above, ranges[range]);
        if (range == 0) {
            // Snow on the far peak, catching the last light.
            Array<PrVec2> snow;
            for (float x = 440; x <= 600; x += 3)
                snow.push(PrVec2{x, horizon - hills(0, x)});
            for (float x = 600; x >= 440; x -= 3) {
                float line = 152 + 9 * prF32Sin(x * .19f) + 4 * prF32Sin(x * .47f + 1);
                snow.push(PrVec2{x, horizon - min(hills(0, x), line)});
            }
            s.polygon(snow, fade(PrColor{.85f, .74f, .80f, 1}, .35f));
        }
        if (range == 1)
            pagoda(s, PrVec2{186, horizon - hills(1, 186) + 3}, PrColor{.20f, .17f, .28f, 1});
        for (int i = 0; i < 4; ++i) {
            float x = s.wrap(t * (4 + float(range) * 2) + float(i * 290 + range * 130), 220);
            s.push(PrAffine2{5, 0, 0, .55f, x, horizon - 12 - float(2 - range) * 18});
            s.glow(PrVec2{0, 0}, 44, fade(PrColor{.78f, .70f, .80f, 1}, .30f));
            s.pop();
        }
    }

    // The moon's path across the water, which shimmers, and faint ripples.
    s.push(PrAffine2{.6f, 0, 0, 2.4f, moon.x, horizon + 90});
    s.glow(PrVec2{0, 0}, 80, fade(moonWhite, .22f));
    s.pop();
    for (int k = 0; k < 26; ++k) {
        float y = horizon + 8 + float(k) * 8 + float(k * k) * .12f;
        float swell = prF32Sin(t * .9f + float(k) * 1.7f);
        float reach = (10 + 18 * swell * swell) * (1 - float(k) / 34);
        float x = moon.x + 4 * prF32Sin(t * .6f + float(k) * .9f);
        s.capsule(PrVec2{x - reach, y}, PrVec2{x + reach, y}, .6f + .25f * float(k % 3),
            fade(moonWhite, .55f * (1 - float(k) / 28)));
    }
    for (float down = horizon; down < s.view.bottom; down += 270) {
        s.across(960, [&](float left) {
            for (int i = 0; i < 30; ++i) {
                float y = down + 14 + float((i * 53) % 270);
                float x = left + float((i * 137) % 960) + 10 * prF32Sin(t * .3f + float(i));
                float reach = 14 + float((i * 29) % 40);
                s.capsule(PrVec2{x - reach, y}, PrVec2{x + reach, y}, .5f,
                    fade(kinari, .05f + .04f * prF32Sin(t * .8f + float(i))));
            }
        });
    }

    // The near shore, and a stone lantern on it. It falls away to the right until it passes under the view.
    Array<PrVec2> shore({{min(s.view.left, 0.0f), s.view.bottom}});
    for (float x = min(s.view.left, 0.0f);; x += 10) {
        float fall = max(x, 0.0f);
        shore.push(PrVec2{x, 586 + fall * fall / 1300 + 5 * prF32Sin(x * .05f)});
        if (shore.last().y > s.view.bottom + 6) {
            shore.push(PrVec2{x, s.view.bottom + 6});
            break;
        }
    }
    s.polygon(shore, PrColor{.025f, .025f, .05f, 1});
    lantern(s, PrVec2{136, 600}, PrColor{.035f, .035f, .065f, 1}, .82f + .1f * prF32Sin(t * 7.3f) + .08f * prF32Sin(t * 13.1f));

    // Maple leaves falling slowly, turning as they fall.
    s.across(1000, [&](float left) {
        for (int i = 0; i < 11; ++i) {
            float period = 15 + float(i % 4) * 4;
            float phase = prF32Fmod(t / period + float(i) * .137f, 1.0f);
            float top = s.view.top - 20;
            float land = 470 + float((i * 67) % 200);
            PrVec2 p{
                left + float((i * 173 + 120) % 1000) - phase * 150 + 26 * prF32Sin(phase * 7 + float(i)),
                top + phase * (land - top),
            };
            float alpha = min(1.0f, min(phase * 8, (1 - phase) * 6));
            float tumble = .35f + .65f * prF32Abs(prF32Sin(t * .9f + float(i)));
            float angle = t * (i % 2 ? .7f : -.6f) + float(i);
            s.push(turn(angle, p, 1, tumble));
            mapleLeaf(s, 8 + float(i % 3) * 1.5f, fade(i % 3 == 0 ? shu : momiji, .9f * alpha));
            s.pop();
        }
    });

    // Shade behind the columns on the right, so that they read against the sky.
    const PrDrawColorStop shade[] = {
        {.offset = 0, .color = fade(indigo, 0)},
        {.offset = 1, .color = fade(indigo, .5f)},
    };
    PrDrawBrush shadeBrush = prDrawBrushLinear(PrVec2{560, 0}, PrVec2{900, 0}, shade, 2);
    prDrawListRect(s.list, PrRect{560, s.view.top, s.view.right, horizon}, {.brush = &shadeBrush});

    // The title and its reading, with the paragraph to their left.
    upright(s, verticalStyle(50, 300, kinari), "月見の庭", PrVec2{884, 70}, 16);
    TextStyle romaji;
    romaji.size = 12.5f;
    romaji.paint.color = kincha;
    const PrFontVariation serif[] = {{PR_FONT_TAG('w', 'g', 'h', 't'), 420}};
    romaji.font = fontVaried("NotoSerif[wdth,wght].ttf", serif, 1);
    const PrTextFeature smallCaps{PR_FONT_TAG('s', 'm', 'c', 'p'), 1, 0, ~0u};
    romaji.features = &smallCaps;
    romaji.featureCount = 1;
    float down = sideways(s, romaji, "Tsukimi no Niwa", PrVec2{830, 74}, 2.6f);
    s.capsule(PrVec2{830, 74 + down + 14}, PrVec2{830, 74 + down + 15}, 1.6f, shu);
    s.rect(806, 82, .8f, 250, fade(kincha, .45f));
    paragraph(s, verticalStyle(18, 330, fade(kinari, .9f)), "山の端《は》に月がのぼり、池の面《おも》に淡い光が落ちる。風が止み、紅葉《もみじ》がひとひら、"
        "またひとひらと水に浮かぶ。ここでは、時がゆっくりと流れている。", PrVec2{774, 84}, 12, 36);

    // Bashō's verse, in one column, with his name and a seal.
    TextStyle verse = verticalStyle(20, 300, kinari);
    float verseEnd = 82 + upright(s, verse, "この道や　行く人なしに　秋の暮", PrVec2{72, 82}, 3);
    TextStyle name = verticalStyle(13, 400, kincha);
    upright(s, name, "芭蕉", PrVec2{38, verseEnd - 30}, 4);
    seal(s, PrVec2{38, verseEnd + 18}, 24, "月庭", shu, PrColor{.10f, .09f, .17f, 1});

    // The hour, in the water's corner: the season, the date with its numbers set across, and the hour of the Rooster.
    TextStyle hour = verticalStyle(14, 400, fade(kincha, .9f));
    PrVec2 at{906, 480};
    at.y += upright(s, hour, "秋・", at);
    combined(s, hour, "10", at);
    at.y += hour.size;
    at.y += upright(s, hour, "月", at);
    combined(s, hour, "25", at);
    at.y += hour.size;
    at.y += upright(s, hour, "日・", at);
    TextStyle hourReading = rubyStyle(hour);
    ruby(s, hourReading, "とり", PrVec2{at.x + hour.size / 2 + hourReading.size / 2 + .5f, at.y}, hour.size);
    upright(s, hour, "酉の刻", at);
}

template <void (*scene)(const Stage&)>
void drawScene(PrDrawList* list, float width, float height, double time, const SceneInput& input) {
    prDrawListPushClipRect(list, PrRect{0, 0, width, height}, PrDrawEdges_Aliased);
    Placement placed = fit(width, height);
    prDrawListPushTransformAffine2(list, PrAffine2{placed.scale, 0, 0, placed.scale, placed.offset.x, placed.offset.y});
    PrRect view{
        -placed.offset.x / placed.scale, -placed.offset.y / placed.scale,
        (width - placed.offset.x) / placed.scale, (height - placed.offset.y) / placed.scale,
    };
    float t = float(time);
    PrVec2 at;
    if (input.over)
        at = PrVec2{
            (input.pointer.x - placed.offset.x) / placed.scale,
            (input.pointer.y - placed.offset.y) / placed.scale,
        };
    else
        at = PrVec2{480 + 230 * prF32Sin(t * .41f), 330 + 110 * prF32Sin(t * .67f + 1)};
    scene(Stage{list, t, view, at, input.hold});
    prDrawListPopTransform(list);
    prDrawListPopClip(list);
}

} // namespace

void scenesOpen(PrTextShaper* shaper, const char* fonts) {
    textShaper = shaper;
    fontDirectory = fonts;
    glyphBuffer = prTextGlyphBufferCreate();
}

void scenesClose() {
    for (const PrDrawImage*& image : cachedFish) {
        prDrawImageRelease(image);
        image = nullptr;
    }
    prDrawImageRelease(cachedWater);
    prDrawImageRelease(cachedSkin);
    cachedWater = cachedSkin = nullptr;
    for (LoadedFont& loaded : loadedFonts())
        prFontRelease(loaded.font);
    loadedFonts().clear();
    prTextGlyphBufferDestroy(glyphBuffer);
    glyphBuffer = nullptr;
}

const Scene* allScenes(uint32_t* count) {
    static const Scene scenes[] = {
        {"Gauges", "Rings, ticks and capsules of a fitness dashboard.", drawScene<gauges>},
        {
            "Theatre", "A paper stage: curtains of polygons, a moon, clipped scenery and a gilded nine-slice.",
            drawScene<theatre>,
        },
        {"Koi", "Fish images turning over a pond of tiled ripples, lily pads, and a magnifying lens.", drawScene<koi>},
        {"Reactor", "Glows, rings and beams of translucent capsules over a starfield.", drawScene<reactor>},
        {"Combat", "A game HUD: a horizon of lines, a ship, a tracking reticle and a radar sweep.", drawScene<combat>},
        {"Jelly", "Superellipse creatures that squash and bounce under turning transforms.", drawScene<jelly>},
        {"Chroma", "Silk strands stroked with one gradient mixed in each color space.", drawScene<chroma>},
        {"Airmail", "Postcards with borders of every style, shaped corners and turned cards.", drawScene<airmail>},
        {
            "Sunroom", "A radio of rounded boxes, drop and inner shadows, and a clipped speaker grille.",
            drawScene<sunroom>,
        },
        {
            "Tsukimi", "Moonrise over still water, set in vertical Japanese: kinsoku breaks, ruby, tate-chu-yoko and a seal.",
            drawScene<tsukimi>,
        },
    };
    *count = countOf(scenes);
    return scenes;
}

void drawCaption(PrDrawList* list, float width, float height, uint32_t scene, bool paused) {
    uint32_t count = 0;
    const Scene& current = allScenes(&count)[scene];

    const PrFontVariation semibold{PR_FONT_TAG('w', 'g', 'h', 't'), 600};
    TextStyle title;
    title.font = fontVaried("NotoSans[wdth,wght].ttf", &semibold, 1);
    title.size = 15;
    title.paint.color = kWhite;
    TextStyle place = title;
    place.paint.color = fade(kWhite, .6f);
    TextStyle hint;
    hint.font = fontNamed("NotoSans[wdth,wght].ttf");
    hint.size = 12;
    hint.paint.color = fade(kWhite, .7f);

    String number = format("   {} / {}", scene + 1, count);
    const char* keys = paused ? "Left and Right arrows: scenes  ·  Space: resume  ·  Esc: quit" :
        "Left and Right arrows: scenes  ·  Space: pause  ·  Esc: quit";
    float nameWidth = textAdvance(title, current.name);
    float lineWidth = nameWidth + textAdvance(place, number);
    float hintWidth = textAdvance(hint, keys);

    // A rounded bar in the band, with a chevron at each end for the arrow keys.
    constexpr float kChevronSpace = 40;
    constexpr float kMargin = 8;
    float barWidth = min(max(lineWidth, hintWidth) + 2 * kChevronSpace, width - 2 * kMargin);
    PrRect bar{(width - barWidth) / 2, height - kCaptionBand + kMargin, (width + barWidth) / 2, height - kMargin};
    prDrawListCornerRect(list, prCornerRectRound(bar, 12), {.color = PrColor{0, 0, 0, .62f}});
    PrDrawStroke stroke = strokeOf(2, PrPathJoin_Round, PrPathCap_Round);
    float middle = (bar.top + bar.bottom) / 2;
    const float sides[] = {-1.0f, 1.0f};
    for (float side : sides) {
        float x = side < 0 ? bar.left + kChevronSpace / 2 + 3 : bar.right - kChevronSpace / 2 - 3;
        PrVec2 points[] = {{x - 3 * side, middle - 7}, {x + 3 * side, middle}, {x - 3 * side, middle + 7}};
        prDrawListPolylineContour(list, points, countOf(points), PrDrawContour_Open, PrPathFillRule_NonZero,
            {.color = fade(kWhite, .8f), .stroke = &stroke});
    }

    float lineLeft = (width - lineWidth) / 2;
    drawText(list, title, current.name, PrVec2{lineLeft, bar.top + 22});
    drawText(list, place, number, PrVec2{lineLeft + nameWidth, bar.top + 22});
    drawText(list, hint, keys, PrVec2{(width - hintWidth) / 2, bar.top + 41});
}

} // namespace scenes
