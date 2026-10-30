// The 2D rendering context of a canvas: CanvasRenderingContext2D, and the paths, gradients and patterns that it draws.
//
// html - https://html.spec.whatwg.org/
//
// A context draws into a bitmap: pixels in memory, or a texture on a device. The context records its drawing into draw
// lists. The renderer that presents the context draws each recording into the bitmap once, as a group with the bitmap
// as its layer (PrDrawGroupLayer). The calls that read or write the context's pixels (prCanvasGetImageData,
// prCanvasPutImageData, prCanvasSnapshot, and the calls that draw the bitmap) first draw what the bitmap does not hold
// yet, with a renderer of the context's own, and then see everything that the context drew. That renderer is made the
// first time that the pixels are read or written. Whichever renderer draws a recording first draws it, and no other
// does. A context and the objects that it draws are used by one thread at a time.
//
// A context resolves CSS values as its canvas element's style resolves them: currentcolor is the element's color, and
// relative font sizes are relative to its font. A context without an element resolves them against the initial values.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/dom.h"
#include "pr/draw.h"
#include "pr/draw_image.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/font_set.h"
#include "pr/geom.h"
#include "pr/gpu.h"
#include "pr/layout_style.h"
#include "pr/locale.h"
#include "pr/path.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The interfaces that this header declares, each named for its web interface.
typedef struct PrCanvas PrCanvas;
typedef struct PrCanvasPath2D PrCanvasPath2D;
typedef struct PrCanvasGradient PrCanvasGradient;
typedef struct PrCanvasPattern PrCanvasPattern;

// ---------------------------------------------------------------------------------------------------------------------
// Settings (html/#canvasrenderingcontext2dsettings)

// The type of the components of a bitmap's pixels (html/#canvascolortype).
enum {
    PrCanvasColorType_Unorm8,       // 8-bit unsigned normalized
    PrCanvasColorType_Float16,      // 16-bit floating point
};
typedef uint8_t PrCanvasColorType;

// How a context's bitmap is made. `colorSpace` is the PredefinedColorSpace of the bitmap: sRGB or Display P3.
typedef struct PrCanvasSettings {
    bool alpha;                     // false for a bitmap that is always opaque
    bool desynchronized;
    bool willReadFrequently;
    PrColorSpace colorSpace;
    PrCanvasColorType colorType;
} PrCanvasSettings;

// Returns the settings of a context created without options: alpha, sRGB, unorm8, and neither desynchronized nor read
// frequently.
PrCanvasSettings prCanvasSettingsDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// Image sources (html/#canvasimagesource)

// The pixels of a source that a context draws or makes a pattern of: an image element's image, another canvas's bitmap,
// or an image bitmap. `extent` is the source's size in CSS pixels, which may differ from the image's pixels, as a
// density-corrected image's does. A source that is not origin-clean taints the bitmap that it is drawn into.
typedef struct PrCanvasImageSource {
    const PrDrawImage* image;
    PrVec2 extent;
    bool originClean;
} PrCanvasImageSource;

// ---------------------------------------------------------------------------------------------------------------------
// Pixel data (html/#imagedata)

// The format of an ImageData's pixels (html/#imagedatapixelformat): straight-alpha RGBA, as 8-bit unsigned integers or
// as 16-bit floats.
enum {
    PrCanvasPixelFormat_RgbaUnorm8,
    PrCanvasPixelFormat_RgbaFloat16,
};
typedef uint8_t PrCanvasPixelFormat;

// Returns the number of bytes of one pixel of `format`.
uint32_t prCanvasPixelFormatSize(PrCanvasPixelFormat format);

// The pixels of an ImageData: `extent` pixels in rows with no gap between them, in `format`, whose colors are in
// `colorSpace`, sRGB or Display P3.
typedef struct PrCanvasImageData {
    PrUVec2 extent;
    PrCanvasPixelFormat format;
    PrColorSpace colorSpace;
} PrCanvasImageData;

// ---------------------------------------------------------------------------------------------------------------------
// CanvasGradient (html/#canvasgradient)
//
// A gradient is reference counted: the script object and each style that holds it share it. A stop added after the
// gradient becomes a style paints in the draws that follow.

// Create gradients, with one reference for the caller (html/#dom-context-2d-createlineargradient and the methods after
// it). Every argument must be finite. A radial gradient returns OutOfRange for a negative radius, and then writes null.
PrCanvasGradient* prCanvasGradientCreateLinear(double x0, double y0, double x1, double y1);
PrStatus prCanvasGradientCreateRadial(double x0, double y0, double r0, double x1, double y1, double r1,
    PrCanvasGradient* PR_NULLABLE* PR_NONNULL out);
PrCanvasGradient* prCanvasGradientCreateConic(double startAngle, double x, double y);

// Adds or releases a reference. The last release frees the gradient.
void prCanvasGradientRetain(PrCanvasGradient* gradient);
void prCanvasGradientRelease(PrCanvasGradient* PR_NULLABLE gradient);

// Adds a stop of `color`, parsed as a CSS <color>, at `offset` (html/#dom-canvasgradient-addcolorstop). Returns
// OutOfRange if `offset` is outside [0, 1], and Syntax if `color` is not a color. `offset` must not be NaN.
PrStatus prCanvasGradientAddColorStop(PrCanvasGradient* gradient, double offset, PrStringView color);

// ---------------------------------------------------------------------------------------------------------------------
// CanvasPattern (html/#canvaspattern)
//
// A pattern is reference counted, as a gradient is. It holds a copy of its image's pixels.

// How a pattern repeats its image (html/#dom-context-2d-createpattern).
enum {
    PrCanvasRepetition_Repeat,
    PrCanvasRepetition_RepeatX,
    PrCanvasRepetition_RepeatY,
    PrCanvasRepetition_NoRepeat,
};
typedef uint8_t PrCanvasRepetition;

// Creates a pattern of `source`, with one reference for the caller (html/#dom-context-2d-createpattern).
PrCanvasPattern* prCanvasPatternCreate(const PrCanvasImageSource* source, PrCanvasRepetition repetition);

// Adds or releases a reference. The last release frees the pattern.
void prCanvasPatternRetain(PrCanvasPattern* pattern);
void prCanvasPatternRelease(PrCanvasPattern* PR_NULLABLE pattern);

// Sets the transform that the pattern's image is mapped through before the current transform
// (html/#dom-canvaspattern-settransform). A transform with a component that is not finite is ignored.
void prCanvasPatternSetTransform(PrCanvasPattern* pattern, PrDAffine2 transform);

// ---------------------------------------------------------------------------------------------------------------------
// Path2D (html/#path2d)

// Creates an empty path.
PrCanvasPath2D* prCanvasPath2DCreate(void);

// Creates a path from SVG path data, up to its first error (html/#dom-path2d).
PrCanvasPath2D* prCanvasPath2DCreateFromPathData(PrStringView data);

// Creates a copy of `path`.
PrCanvasPath2D* prCanvasPath2DCreateCopy(const PrCanvasPath2D* path);

// Frees the path.
void prCanvasPath2DDestroy(PrCanvasPath2D* PR_NULLABLE path);

// Appends the subpaths of `other`, mapped through `transform` (html/#dom-path2d-addpath). A transform with a component
// that is not finite adds nothing. `other` may be `path`.
void prCanvasPath2DAddPath(PrCanvasPath2D* path, const PrCanvasPath2D* other, PrDAffine2 transform);

// The methods of CanvasPath (html/#canvaspath), on a Path2D, whose coordinates are its own. A method with an argument
// that is not finite adds nothing. arcTo, arc and ellipse return OutOfRange for a negative radius, and roundRect for a
// negative radius or for a count of radii outside 1 through 4, and then add nothing. A radius of roundRect is a
// corner's horizontal and vertical radius.
void prCanvasPath2DClosePath(PrCanvasPath2D* path);
void prCanvasPath2DMoveTo(PrCanvasPath2D* path, double x, double y);
void prCanvasPath2DLineTo(PrCanvasPath2D* path, double x, double y);
void prCanvasPath2DQuadraticCurveTo(PrCanvasPath2D* path, double cpx, double cpy, double x, double y);
void prCanvasPath2DBezierCurveTo(PrCanvasPath2D* path, double cp1x, double cp1y, double cp2x, double cp2y, double x, double y);
PrStatus prCanvasPath2DArcTo(PrCanvasPath2D* path, double x1, double y1, double x2, double y2, double radius);
void prCanvasPath2DRect(PrCanvasPath2D* path, double x, double y, double w, double h);
PrStatus prCanvasPath2DRoundRect(PrCanvasPath2D* path, double x, double y, double w, double h,
    const PrDVec2* PR_NULLABLE radii, uint32_t radiusCount);
PrStatus prCanvasPath2DArc(PrCanvasPath2D* path, double x, double y, double radius, double startAngle,
    double endAngle, bool counterclockwise);
PrStatus prCanvasPath2DEllipse(PrCanvasPath2D* path, double x, double y, double radiusX, double radiusY,
    double rotation, double startAngle, double endAngle, bool counterclockwise);

// ---------------------------------------------------------------------------------------------------------------------
// CanvasRenderingContext2D (html/#canvasrenderingcontext2d)

// Called with `changedContext` the first time that what a context's bitmap shows may have changed after the context was
// created or its bitmap was last taken as an image or presented (prCanvasSnapshot, prCanvasPresent). The owner of a
// context shows the bitmap again at its next opportunity.
typedef void (*PrCanvasChangedFn)(void* PR_NULLABLE context);

// What a context's text is set with: the families of its font source (html/#font-source) and the shaper that shapes
// them, and the language and direction that text takes where its own are inherited (html/#text-preparation-algorithm).
typedef struct PrCanvasTextSource {
    PrFontSet* fonts;
    PrTextShaper* shaper;
    PrLocale language;
    PrTextDirection direction;
} PrCanvasTextSource;

// Writes what a context's text is set with when it draws or measures text, which stays valid for that call. Returns
// false if text has no font source, in which case text draws nothing and measures zero. Called with
// `textSourceContext`.
typedef bool (*PrCanvasTextSourceFn)(void* PR_NULLABLE context, PrCanvasTextSource* out);

// What a context is made from. The bitmap is a texture on `device`, or pixels in memory if `device` is null. The
// renderer that presents the context must render on that device, or in memory if `device` is null. `element` is the
// canvas element whose style CSS values resolve against, or null, and must outlive the context. `changed` is called
// when the bitmap changes, or is null. `textSource` is null for a context that sets no text.
typedef struct PrCanvasDesc {
    PrUVec2 extent;
    PrCanvasSettings settings;
    PrGpuDevice* PR_NULLABLE device;
    const PrDomElement* PR_NULLABLE element;
    PrCanvasChangedFn PR_NULLABLE changed;
    void* PR_NULLABLE changedContext;
    PrCanvasTextSourceFn PR_NULLABLE textSource;
    void* PR_NULLABLE textSourceContext;
} PrCanvasDesc;

// Creates a context whose bitmap is transparent black, or opaque black without alpha, and whose state has its initial
// values.
PrCanvas* prCanvasCreate(const PrCanvasDesc* desc);

// Frees the context.
void prCanvasDestroy(PrCanvas* PR_NULLABLE canvas);

// --- The bitmap ---

// The largest side of a bitmap that a context allocates. A larger bitmap has no pixels: nothing draws into it, and its
// pixels read as transparent black.
enum { kPrCanvasBitmapMaxSize = 16384 };

// Returns the extent of the bitmap, in pixels.
PrUVec2 prCanvasExtent(const PrCanvas* canvas);

// Resets the context to its default state and resizes its bitmap (html/#concept-canvas-set-bitmap-dimensions).
void prCanvasSetExtent(PrCanvas* canvas, PrUVec2 extent);

// Returns a number that changes whenever what the bitmap shows may have changed.
uint64_t prCanvasVersion(const PrCanvas* canvas);

// Returns the bitmap's pixels as an image, premultiplied in extended sRGB, with one reference for the caller. Returns
// null for a bitmap without pixels. The image is the same until the bitmap changes.
const PrDrawImage* PR_NULLABLE prCanvasSnapshot(PrCanvas* canvas);

// What a context shows as of a presentation: a painter that draws the bitmap as it is then, scaled over the extent that
// it is shown at and sampled as the presentation says, and the revision of what it draws. The painter holds what it
// draws, and may be called on any thread. A later presentation of a bitmap that changed has a later revision.
typedef struct PrCanvasPresentation {
    PrLayoutPainter painter;        // null for a bitmap without pixels
    uint64_t revision;
} PrCanvasPresentation;

// Presents what the bitmap shows now. The painter's drawing draws what the context recorded into the bitmap, each
// recording once, however many renderers or frames draw it, and draws the bitmap.
PrCanvasPresentation prCanvasPresent(PrCanvas* canvas, PrDrawSampling sampling);

// Returns false once the bitmap has drawn a source that is not origin-clean (html/#concept-canvas-origin-clean).
bool prCanvasIsOriginClean(const PrCanvas* canvas);

// --- CanvasSettings ---

// Returns the settings that the context was created with (html/#dom-context-2d-canvas-getcontextattributes).
PrCanvasSettings prCanvasGetContextAttributes(const PrCanvas* canvas);

// --- CanvasState ---

// Push and pop the drawing state (html/#dom-context-2d-save, html/#dom-context-2d-restore), and reset the context to
// its default state, its bitmap cleared (html/#dom-context-2d-reset). A restore without a saved state does nothing.
void prCanvasSave(PrCanvas* canvas);
void prCanvasRestore(PrCanvas* canvas);
void prCanvasReset(PrCanvas* canvas);

// Returns true if the context was lost (html/#dom-context-2d-iscontextlost). A context in memory is never lost.
bool prCanvasIsContextLost(const PrCanvas* canvas);

// --- CanvasTransform ---

// The methods that change the current transform (html/#dom-context-2d-scale and the methods after it). A method with an
// argument that is not finite does nothing. An angle is in radians, clockwise.
void prCanvasScale(PrCanvas* canvas, double x, double y);
void prCanvasRotate(PrCanvas* canvas, double angle);
void prCanvasTranslate(PrCanvas* canvas, double x, double y);
void prCanvasTransform(PrCanvas* canvas, PrDAffine2 transform);
void prCanvasSetTransform(PrCanvas* canvas, PrDAffine2 transform);
void prCanvasResetTransform(PrCanvas* canvas);

// Returns the current transform (html/#dom-context-2d-gettransform).
PrDAffine2 prCanvasGetTransform(const PrCanvas* canvas);

// --- CanvasCompositing ---

// Get and set the global alpha (html/#dom-context-2d-globalalpha). A value outside [0, 1] or not finite is ignored.
double prCanvasGlobalAlpha(const PrCanvas* canvas);
void prCanvasSetGlobalAlpha(PrCanvas* canvas, double value);

// Get and set the compositing and blending operator by its name (html/#dom-context-2d-globalcompositeoperation). A name
// that is not an operator's is ignored. The name lives as long as the process.
PrStringView prCanvasGlobalCompositeOperation(const PrCanvas* canvas);
void prCanvasSetGlobalCompositeOperation(PrCanvas* canvas, PrStringView value);

// --- CanvasImageSmoothing ---

// The quality of image smoothing (html/#imagesmoothingquality).
enum {
    PrCanvasImageSmoothingQuality_Low,
    PrCanvasImageSmoothingQuality_Medium,
    PrCanvasImageSmoothingQuality_High,
};
typedef uint8_t PrCanvasImageSmoothingQuality;

// Get and set whether images are smoothed when scaled, and how (html/#dom-context-2d-imagesmoothingenabled,
// html/#dom-context-2d-imagesmoothingquality).
bool prCanvasImageSmoothingEnabled(const PrCanvas* canvas);
void prCanvasSetImageSmoothingEnabled(PrCanvas* canvas, bool value);
PrCanvasImageSmoothingQuality prCanvasImageSmoothingQuality(const PrCanvas* canvas);
void prCanvasSetImageSmoothingQuality(PrCanvas* canvas, PrCanvasImageSmoothingQuality value);

// --- CanvasFillStrokeStyles ---

// The style that a draw paints with: the fill style or the stroke style.
enum {
    PrCanvasStyleRole_Fill,
    PrCanvasStyleRole_Stroke,
};
typedef uint8_t PrCanvasStyleRole;

// The kinds of style.
enum {
    PrCanvasStyleKind_Color,
    PrCanvasStyleKind_Gradient,
    PrCanvasStyleKind_Pattern,
};
typedef uint8_t PrCanvasStyleKind;

// A style as html/#dom-context-2d-fillstyle returns it. Its kind decides which field it uses. The color is its
// serialization, which the context owns until the style changes.
typedef struct PrCanvasStyle {
    PrCanvasStyleKind kind;
    PrStringView color;                         // Color
    PrCanvasGradient* PR_NULLABLE gradient;     // Gradient
    PrCanvasPattern* PR_NULLABLE pattern;       // Pattern
} PrCanvasStyle;

// Returns the fill or stroke style.
PrCanvasStyle prCanvasStyle(const PrCanvas* canvas, PrCanvasStyleRole role);

// Set the fill or stroke style (html/#dom-context-2d-fillstyle). A color is parsed as a CSS <color>, and ignored if it
// is not one. The style holds a reference to a gradient or a pattern. A pattern that is not origin-clean taints the
// bitmap.
void prCanvasSetStyleColor(PrCanvas* canvas, PrCanvasStyleRole role, PrStringView color);
void prCanvasSetStyleGradient(PrCanvas* canvas, PrCanvasStyleRole role, PrCanvasGradient* gradient);
void prCanvasSetStylePattern(PrCanvas* canvas, PrCanvasStyleRole role, PrCanvasPattern* pattern);

// --- CanvasShadowStyles ---

// Get and set the shadow's offset and blur (html/#dom-context-2d-shadowoffsetx and the attributes after it). A value
// that is not finite is ignored, and so is a negative blur.
PrDVec2 prCanvasShadowOffset(const PrCanvas* canvas);
void prCanvasSetShadowOffset(PrCanvas* canvas, PrDVec2 value);
double prCanvasShadowBlur(const PrCanvas* canvas);
void prCanvasSetShadowBlur(PrCanvas* canvas, double value);

// Get and set the shadow's color, as the fill style's color is (html/#dom-context-2d-shadowcolor). The context owns the
// serialization until the color changes.
PrStringView prCanvasShadowColor(const PrCanvas* canvas);
void prCanvasSetShadowColor(PrCanvas* canvas, PrStringView value);

// --- CanvasFilters ---

// Get and set the filter, a CSS <filter-value-list> or "none" (html/#dom-context-2d-filter). A value that does not
// parse is ignored. The context owns the text until the filter changes.
PrStringView prCanvasFilter(const PrCanvas* canvas);
void prCanvasSetFilter(PrCanvas* canvas, PrStringView value);

// --- CanvasRect ---

// Clear, fill and stroke a rectangle (html/#dom-context-2d-clearrect and the methods after it). A rectangle with an
// argument that is not finite draws nothing.
void prCanvasClearRect(PrCanvas* canvas, double x, double y, double w, double h);
void prCanvasFillRect(PrCanvas* canvas, double x, double y, double w, double h);
void prCanvasStrokeRect(PrCanvas* canvas, double x, double y, double w, double h);

// --- CanvasDrawPath ---

// Empties the current default path (html/#dom-context-2d-beginpath).
void prCanvasBeginPath(PrCanvas* canvas);

// Fill, stroke and clip to `path`, or to the current default path if `path` is null (html/#dom-context-2d-fill and the
// methods after it).
void prCanvasFill(PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path, PrPathFillRule rule);
void prCanvasStroke(PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path);
void prCanvasClip(PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path, PrPathFillRule rule);

// Return true if the point, in the bitmap's coordinates unaffected by the current transform, is inside `path` or its
// stroke, or the current default path's if `path` is null (html/#dom-context-2d-ispointinpath,
// html/#dom-context-2d-ispointinstroke). A point that is not finite is inside neither.
bool prCanvasIsPointInPath(const PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path, double x, double y,
    PrPathFillRule rule);
bool prCanvasIsPointInStroke(const PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path, double x, double y);

// --- CanvasUserInterface ---

// Draws a focus ring around `path`, or the current default path if `path` is null, as drawFocusIfNeeded does for a
// focused element (html/#dom-context-2d-drawfocusifneeded). The caller decides whether the element is focused.
void prCanvasDrawFocusRing(PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path);

// --- CanvasText ---

// The measurements of text (html/#textmetrics), in CSS pixels. Each vertical measurement is from the baseline that the
// text baseline names, up for an ascent and down for a descent, and each baseline's is how far the named baseline lies
// below it.
typedef struct PrCanvasTextMetrics {
    double width;
    double actualBoundingBoxLeft;
    double actualBoundingBoxRight;
    double fontBoundingBoxAscent;
    double fontBoundingBoxDescent;
    double actualBoundingBoxAscent;
    double actualBoundingBoxDescent;
    double emHeightAscent;
    double emHeightDescent;
    double hangingBaseline;
    double alphabeticBaseline;
    double ideographicBaseline;
} PrCanvasTextMetrics;

// Fill or stroke `text` at (x, y), condensed to `maxWidth` if it is not null and the text is wider
// (html/#dom-context-2d-filltext). An argument that is not finite, or a maximum width of zero or less, draws nothing.
void prCanvasFillText(PrCanvas* canvas, PrStringView text, double x, double y, const double* PR_NULLABLE maxWidth);
void prCanvasStrokeText(PrCanvas* canvas, PrStringView text, double x, double y, const double* PR_NULLABLE maxWidth);

// Returns the measurements of `text` in the current text styles (html/#dom-context-2d-measuretext).
PrCanvasTextMetrics prCanvasMeasureText(PrCanvas* canvas, PrStringView text);

// --- CanvasDrawImage ---

// Draws the rectangle (sx, sy, sw, sh) of `source`, in its CSS pixels, into the rectangle (dx, dy, dw, dh)
// (html/#dom-context-2d-drawimage). A rectangle's width and height may be negative. An argument that is not finite, or
// a rectangle with no area, draws nothing.
void prCanvasDrawImage(PrCanvas* canvas, const PrCanvasImageSource* source,
    double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh);

// --- CanvasImageData ---

// Writes the pixels of `rect` of the bitmap to `out`, as `data` describes them, with `data->extent` the rect's extent
// (html/#dom-context-2d-getimagedata). Pixels outside the bitmap are transparent black. `out` holds the pixels with no
// gap between rows.
void prCanvasGetImageData(PrCanvas* canvas, PrIVec2 origin, const PrCanvasImageData* data, void* out);

// Writes the pixels of the dirty rectangle (dirtyX, dirtyY, dirtyWidth, dirtyHeight) of `pixels`, which `data`
// describes, into the bitmap with the top left of `pixels` at `origin`, replacing what the bitmap holds
// (html/#dom-context-2d-putimagedata). The dirty rectangle's width and height may be negative, and it is clipped to the
// pixels and to the bitmap.
void prCanvasPutImageData(PrCanvas* canvas, const PrCanvasImageData* data,
    const void* pixels, PrIVec2 origin, int32_t dirtyX, int32_t dirtyY, int32_t dirtyWidth, int32_t dirtyHeight);

// --- CanvasPathDrawingStyles ---

// The ends of lines (html/#canvaslinecap), and the corners where they meet (html/#canvaslinejoin).
enum {
    PrCanvasLineCap_Butt,
    PrCanvasLineCap_Round,
    PrCanvasLineCap_Square,
};
typedef uint8_t PrCanvasLineCap;
enum {
    PrCanvasLineJoin_Round,
    PrCanvasLineJoin_Bevel,
    PrCanvasLineJoin_Miter,
};
typedef uint8_t PrCanvasLineJoin;

// Get and set the line width and the miter limit (html/#dom-context-2d-linewidth, html/#dom-context-2d-miterlimit). A
// value that is not finite, or not above zero, is ignored.
double prCanvasLineWidth(const PrCanvas* canvas);
void prCanvasSetLineWidth(PrCanvas* canvas, double value);
double prCanvasMiterLimit(const PrCanvas* canvas);
void prCanvasSetMiterLimit(PrCanvas* canvas, double value);

// Get and set the line cap and join (html/#dom-context-2d-linecap, html/#dom-context-2d-linejoin).
PrCanvasLineCap prCanvasLineCap(const PrCanvas* canvas);
void prCanvasSetLineCap(PrCanvas* canvas, PrCanvasLineCap value);
PrCanvasLineJoin prCanvasLineJoin(const PrCanvas* canvas);
void prCanvasSetLineJoin(PrCanvas* canvas, PrCanvasLineJoin value);

// Set and get the dash list (html/#dom-context-2d-setlinedash, html/#dom-context-2d-getlinedash). A list with a value
// that is negative or not finite is ignored, and a list of an odd count is used twice over. The dashes that the getter
// returns are valid until the dash list changes.
void prCanvasSetLineDash(PrCanvas* canvas, const double* PR_NULLABLE segments, uint32_t count);
uint32_t prCanvasLineDashCount(const PrCanvas* canvas);
const double* PR_NULLABLE prCanvasGetLineDash(const PrCanvas* canvas);

// Get and set the dash offset (html/#dom-context-2d-linedashoffset). A value that is not finite is ignored.
double prCanvasLineDashOffset(const PrCanvas* canvas);
void prCanvasSetLineDashOffset(PrCanvas* canvas, double value);

// --- CanvasTextDrawingStyles ---

// The values of the text styles' enumerations (html/#canvastextalign and the enumerations after it).
enum {
    PrCanvasTextAlign_Start,
    PrCanvasTextAlign_End,
    PrCanvasTextAlign_Left,
    PrCanvasTextAlign_Right,
    PrCanvasTextAlign_Center,
};
typedef uint8_t PrCanvasTextAlign;
enum {
    PrCanvasTextBaseline_Top,
    PrCanvasTextBaseline_Hanging,
    PrCanvasTextBaseline_Middle,
    PrCanvasTextBaseline_Alphabetic,
    PrCanvasTextBaseline_Ideographic,
    PrCanvasTextBaseline_Bottom,
};
typedef uint8_t PrCanvasTextBaseline;
enum {
    PrCanvasDirection_Ltr,
    PrCanvasDirection_Rtl,
    PrCanvasDirection_Inherit,
};
typedef uint8_t PrCanvasDirection;
enum {
    PrCanvasFontKerning_Auto,
    PrCanvasFontKerning_Normal,
    PrCanvasFontKerning_None,
};
typedef uint8_t PrCanvasFontKerning;
enum {
    PrCanvasFontStretch_UltraCondensed,
    PrCanvasFontStretch_ExtraCondensed,
    PrCanvasFontStretch_Condensed,
    PrCanvasFontStretch_SemiCondensed,
    PrCanvasFontStretch_Normal,
    PrCanvasFontStretch_SemiExpanded,
    PrCanvasFontStretch_Expanded,
    PrCanvasFontStretch_ExtraExpanded,
    PrCanvasFontStretch_UltraExpanded,
};
typedef uint8_t PrCanvasFontStretch;
enum {
    PrCanvasFontVariantCaps_Normal,
    PrCanvasFontVariantCaps_SmallCaps,
    PrCanvasFontVariantCaps_AllSmallCaps,
    PrCanvasFontVariantCaps_PetiteCaps,
    PrCanvasFontVariantCaps_AllPetiteCaps,
    PrCanvasFontVariantCaps_Unicase,
    PrCanvasFontVariantCaps_TitlingCaps,
};
typedef uint8_t PrCanvasFontVariantCaps;
enum {
    PrCanvasTextRendering_Auto,
    PrCanvasTextRendering_OptimizeSpeed,
    PrCanvasTextRendering_OptimizeLegibility,
    PrCanvasTextRendering_GeometricPrecision,
};
typedef uint8_t PrCanvasTextRendering;

// Get and set the language, a BCP 47 tag, the empty string, or "inherit" (html/#dom-context-2d-lang), and the font, a
// value of the CSS font shorthand that is ignored if it does not parse (html/#dom-context-2d-font). The context owns
// the text until it changes.
PrStringView prCanvasLang(const PrCanvas* canvas);
void prCanvasSetLang(PrCanvas* canvas, PrStringView value);
PrStringView prCanvasFont(const PrCanvas* canvas);
void prCanvasSetFont(PrCanvas* canvas, PrStringView value);

// Get and set the text's alignment, baseline and direction (html/#dom-context-2d-textalign and the attributes after
// it).
PrCanvasTextAlign prCanvasTextAlign(const PrCanvas* canvas);
void prCanvasSetTextAlign(PrCanvas* canvas, PrCanvasTextAlign value);
PrCanvasTextBaseline prCanvasTextBaseline(const PrCanvas* canvas);
void prCanvasSetTextBaseline(PrCanvas* canvas, PrCanvasTextBaseline value);
PrCanvasDirection prCanvasDirection(const PrCanvas* canvas);
void prCanvasSetDirection(PrCanvas* canvas, PrCanvasDirection value);

// Get and set the spacing after each character and after each word, a CSS <length> that is ignored if it does not parse
// (html/#dom-context-2d-letterspacing, html/#dom-context-2d-wordspacing). A length relative to the font resolves
// against the font that the text is drawn in. The context owns the text until it changes.
PrStringView prCanvasLetterSpacing(const PrCanvas* canvas);
void prCanvasSetLetterSpacing(PrCanvas* canvas, PrStringView value);
PrStringView prCanvasWordSpacing(const PrCanvas* canvas);
void prCanvasSetWordSpacing(PrCanvas* canvas, PrStringView value);

// Get and set the font's kerning, width, capitals and rendering (html/#dom-context-2d-fontkerning and the attributes
// after it). A width or capitals other than normal override the font's.
PrCanvasFontKerning prCanvasFontKerning(const PrCanvas* canvas);
void prCanvasSetFontKerning(PrCanvas* canvas, PrCanvasFontKerning value);
PrCanvasFontStretch prCanvasFontStretch(const PrCanvas* canvas);
void prCanvasSetFontStretch(PrCanvas* canvas, PrCanvasFontStretch value);
PrCanvasFontVariantCaps prCanvasFontVariantCaps(const PrCanvas* canvas);
void prCanvasSetFontVariantCaps(PrCanvas* canvas, PrCanvasFontVariantCaps value);
PrCanvasTextRendering prCanvasTextRendering(const PrCanvas* canvas);
void prCanvasSetTextRendering(PrCanvas* canvas, PrCanvasTextRendering value);

// --- CanvasPath ---

// The methods of CanvasPath on the current default path, whose points are mapped through the current transform as they
// are added. Each takes its arguments and returns its status as the Path2D method of its name does.
void prCanvasClosePath(PrCanvas* canvas);
void prCanvasMoveTo(PrCanvas* canvas, double x, double y);
void prCanvasLineTo(PrCanvas* canvas, double x, double y);
void prCanvasQuadraticCurveTo(PrCanvas* canvas, double cpx, double cpy, double x, double y);
void prCanvasBezierCurveTo(PrCanvas* canvas, double cp1x, double cp1y, double cp2x, double cp2y, double x, double y);
PrStatus prCanvasArcTo(PrCanvas* canvas, double x1, double y1, double x2, double y2, double radius);
void prCanvasRect(PrCanvas* canvas, double x, double y, double w, double h);
PrStatus prCanvasRoundRect(PrCanvas* canvas, double x, double y, double w,
    double h, const PrDVec2* PR_NULLABLE radii, uint32_t radiusCount);
PrStatus prCanvasArc(PrCanvas* canvas, double x, double y, double radius,
    double startAngle, double endAngle, bool counterclockwise);
PrStatus prCanvasEllipse(PrCanvas* canvas, double x, double y, double radiusX,
    double radiusY, double rotation, double startAngle, double endAngle, bool counterclockwise);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
