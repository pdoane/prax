// Drawings: scenes drawn in code, each a list feature or a workload, and the toolkit that they share.

#pragma once

#include "pr/app_display.h"
#include "pr/array.h"
#include "pr/draw_filter.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/min_max.h"
#include "pr/text.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace drawings {

using namespace pr;

// π.
constexpr float kPi = 3.14159265358979f;

// The drawings' palette.
constexpr PrColor kInk{0.10f, 0.10f, 0.12f, 1};
constexpr PrColor kPaper{0.96f, 0.95f, 0.92f, 1};
constexpr PrColor kRed{0.86f, 0.20f, 0.18f, 1};
constexpr PrColor kBlue{0.16f, 0.40f, 0.82f, 1};
constexpr PrColor kGreen{0.20f, 0.62f, 0.32f, 1};
constexpr PrColor kGold{0.93f, 0.66f, 0.12f, 1};
constexpr PrColor kPurple{0.52f, 0.28f, 0.70f, 1};
constexpr PrColor kTeal{0.10f, 0.60f, 0.62f, 1};
constexpr PrColor kWhite{1, 1, 1, 1};
constexpr PrColor kGray{0.55f, 0.55f, 0.58f, 1};

// A drawing: its name, a line describing what it shows, and the function that draws it. `draw` draws at `width` by
// `height` logical pixels, at `time` in seconds. A feature drawing ignores the time, and captions each case it draws.
struct Drawing {
    const char* name;
    const char* description;
    void (*draw)(PrDrawList* list, float width, float height, double time);
};

// The height of the band under a grid cell that holds its caption.
constexpr float kCaptionBand = 16;

// Draws the drawing for `display`, with its text shaped by `shaper` and its hinted fonts hinted by `hinter`, or drawn
// in their design outlines without one, and the captions of its cases over what it draws, without the drawing's state.
void drawDrawing(const Drawing& drawing, PrDrawList* list, PrTextShaper* shaper, PrFontHinter* PR_NULLABLE hinter,
    const PrAppDisplay& display, float width, float height, double time);

// Returns the shaper and the hinter of the drawing that is being drawn, and the display that it is drawn for.
PrTextShaper* drawingShaper();
PrFontHinter* PR_NULLABLE drawingHinter();
const PrAppDisplay& drawingDisplay();

// Captions a case with the text, centered in `band` on a baseline just above the band's bottom. The text shrinks if it
// would not fit. Captions are in the drawing's space.
void caption(PrRect band, PrStringView text);

// Returns the color with its alpha replaced by `alpha`.
inline PrColor withAlpha(PrColor color, float alpha) {
    return PrColor{color.r, color.g, color.b, alpha};
}

// Returns a default stroke of `width`, with `join`, and with `cap` at both ends and on dashes.
inline PrDrawStroke strokeOf(float width, PrPathJoin join = PrPathJoin_Miter, PrPathCap cap = PrPathCap_Butt) {
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    stroke.shape.join = join;
    stroke.shape.startCap = cap;
    stroke.shape.endCap = cap;
    stroke.shape.dashCap = cap;
    return stroke;
}

// Returns a stroke one target pixel wide, regardless of the transform.
inline PrDrawStroke hairlineStroke() {
    PrDrawStroke stroke = strokeOf(1);
    stroke.nonScaling = true;
    return stroke;
}

// A drawing's cells laid out in a grid over it. Each cell is a rectangle to draw one case into, above a band for its
// caption.
struct Grid {
    float width;
    float height;
    uint32_t columns;
    uint32_t rows;

    // Returns the grid's rectangle at `column` and `row`, which holds the cell and its caption band.
    PrRect slot(uint32_t column, uint32_t row) const {
        float w = width / float(columns);
        float h = height / float(rows);
        return PrRect{float(column) * w, float(row) * h, float(column + 1) * w, float(row + 1) * h};
    }

    // Returns the rectangle that the case at `column` and `row` draws into: the slot inset by `inset`, above the
    // caption band.
    PrRect cell(uint32_t column, uint32_t row, float inset = 16) const {
        PrRect s = slot(column, row);
        return PrRect{s.left + inset, s.top + inset, s.right - inset, s.bottom - inset - kCaptionBand};
    }

    // Returns the cell at `index`, counted along the rows.
    PrRect at(uint32_t index, float inset = 16) const {
        return cell(index % columns, index / columns, inset);
    }

    // Captions the cell in the band under it, with the cell's `inset`.
    void caption(uint32_t column, uint32_t row, PrStringView text, float inset = 16) const {
        PrRect s = slot(column, row);
        drawings::caption(PrRect{s.left + 4, s.bottom - inset - kCaptionBand, s.right - 4, s.bottom - inset}, text);
    }

    // Captions the cell at `index`, counted along the rows.
    void captionAt(uint32_t index, PrStringView text, float inset = 16) const {
        caption(index % columns, index / columns, text, inset);
    }
};

// Returns the smaller of the rectangle's width and height.
inline float extentOf(PrRect r) {
    float w = prRectWidth(r);
    float h = prRectHeight(r);
    return w < h ? w : h;
}

// Returns the rectangle moved in by `by` on every side.
inline PrRect inset(PrRect r, float by) {
    return PrRect{r.left + by, r.top + by, r.right - by, r.bottom - by};
}

// Returns the square of `size` centered in the rectangle.
inline PrRect squareIn(PrRect r, float size) {
    PrVec2 c = prRectCenter(r);
    return PrRect{c.x - size / 2, c.y - size / 2, c.x + size / 2, c.y + size / 2};
}

// Returns a star of `points` points around the center. It crosses itself if `step` is above 1.
inline PrPath* star(PrVec2 center, float radius, uint32_t points, uint32_t step) {
    PrPath* path = prPathCreate();
    for (uint32_t i = 0; i < points; ++i) {
        float angle = -kPi / 2 + 2 * kPi * float(i * step % points) / float(points);
        PrVec2 p{center.x + radius * prF32Cos(angle), center.y + radius * prF32Sin(angle)};
        if (i == 0)
            prPathMoveTo(path, p);
        else
            prPathLineTo(path, p);
    }
    prPathClose(path);
    return path;
}

// Records the image's texels in `source` drawn over `destination`, at `opacity`, as a rectangle filled with the image's
// brush.
inline void drawImage(PrDrawList* list, const PrDrawImage* image, PrRect source, PrRect destination,
        PrDrawSampling sampling, float opacity) {
    PrDrawSource from = prDrawSourceImage(image, source);
    PrDrawBrush brush = prDrawBrushImage(from, destination, sampling);
    brush.opacity = opacity;
    prDrawListRect(list, destination, {.brush = &brush});
}

// Records the whole image as a nine-slice over `destination`.
inline void drawNineSlice(PrDrawList* list, const PrDrawImage* image, const PrDrawNineSlice* slices,
        PrRect destination, PrDrawSampling sampling, PrColor tint) {
    PrRect whole{0, 0, float(prDrawImageExtent(image).x), float(prDrawImageExtent(image).y)};
    PrDrawSource from = prDrawSourceImage(image, whole);
    prDrawListNineSlice(list, from, slices, destination, sampling, {.color = tint});
}

// Outlines the cell with a faint hairline.
inline void frameCell(PrDrawList* list, PrRect cell) {
    PrDrawStroke hairline = hairlineStroke();
    prDrawListRect(list, cell, {.color = withAlpha(kInk, 0.15f), .stroke = &hairline});
}

// Returns a corner of `radius` along both axes, with `shape`.
inline PrCorner cornerOf(float radius, float shape) {
    return PrCorner{PrVec2{radius, radius}, shape};
}

// The side lengths, in pixels, of the images that the drawings share.
constexpr uint32_t kTestCardSize = 64;
constexpr uint32_t kFrameImageSize = 48;
constexpr uint32_t kSmallTileSize = 16;

// Images that the drawings share, created once: a colorful test card, a nine-slice frame and a checker with a dot.
const PrDrawImage* testCard();
const PrDrawImage* frameImage();
const PrDrawImage* smallTile();

// Returns the test card encoded as a PNG and decoded back through lib/image, created once. Returns null if lib/image
// cannot read it.
const PrDrawImage* PR_NULLABLE decodedCard();

// Returns a list of a small motif within (0, 0) to (40, 40).
PrDrawList* motif();

// Releases the images and lists above.
void releaseDrawingImages();

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Returns a font from third_party/fonts, loaded once.
const PrFont* fontNamed(const char* file);

// Returns a font at a design point, created once for each design point.
const PrFont* fontVaried(const char* file, const PrFontVariation* PR_NULLABLE values, uint32_t count);

// Returns a font derived as `derivation` specifies, created once for each derivation.
const PrFont* fontDerived(const char* file, const PrFontDerivation& derivation);

// How a run of text is shaped and drawn.
struct TextStyle {
    const PrFont* PR_NULLABLE font = nullptr;
    float size = 16;
    PrDrawPaint paint{.color = kInk};
    PrTextDirection direction = PrTextDirection_Ltr;
    PrTextAxis axis = PrTextAxis_Horizontal;
    PrScript script = PrScript_Zzzz;
    const char* PR_NULLABLE language = nullptr;     // BCP 47
    const PrTextFeature* PR_NULLABLE features = nullptr;
    uint32_t featureCount = 0;
    PrTextKerning kerning = PrTextKerning_Auto;
    PrTextCaps caps = PrTextCaps_Normal;
    PrTextPosition position = PrTextPosition_Normal;
    PrTextSynthesisFlags synthesis = 0;
    uint32_t palette = 0;
};

// Glyphs placed for one run in a style's font.
struct ShapedText {
    Array<PrFontGlyph> glyphs;
    Array<PrAffine2> transforms;    // by glyph: a transform about the glyph's origin, or empty if every glyph is drawn
                                    // at the style's size
};

// Places the text's glyphs on the baseline from `origin`, left to right regardless of the text's direction, or top to
// bottom. A glyph that the shaper synthesized at another size gets a transform that scales it. Returns the advance.
float shapeText(const TextStyle& style, PrStringView text, PrVec2 origin, ShapedText& out);

// Returns the text's advance as shapeText places it.
float textAdvance(const TextStyle& style, PrStringView text);

// Records the glyphs as one run in the style's font.
void drawGlyphs(PrDrawList* list, const TextStyle& style, const ShapedText& shaped);

// Records the text as one glyph run from `origin`. Returns its advance.
float drawText(PrDrawList* list, const TextStyle& style, PrStringView text, PrVec2 origin);

// Draws a label with its baseline at `origin`, over the drawing, in the same way as captions.
void label(PrStringView text, PrVec2 origin, float size = 11, PrColor tint = kGray);

// Releases the fonts and glyph buffer above.
void releaseDrawingText();

} // namespace drawings

PR_ASSUME_NONNULL_END
