// The 2D rendering context of a canvas: CanvasRenderingContext2D, and the paths, gradients and patterns that it draws.
//
// html - https://html.spec.whatwg.org/

#include "pr/canvas.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/draw_filter.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/layout_style.h"
#include "pr/mem.h"
#include "pr/path_stroke.h"
#include "pr/string_ops.h"

#include "canvas/src/canvas_path.h"
#include "canvas/src/canvas_text.h"

namespace pr_impl {

namespace {

// A compositing and blending operator by its name (html/#dom-context-2d-globalcompositeoperation): a <composite-mode>,
// which blends normally, or a <blend-mode>, which composites source-over (compositing-1/#canvascompositingandblending).
struct CanvasOperation {
    const char* name;
    PrDrawComposite composite;
    PrDrawBlendMode blendMode;
};

constexpr CanvasOperation kOperations[] = {
    {"source-over", PrDrawComposite_SourceOver, PrDrawBlendMode_Normal},
    {"source-in", PrDrawComposite_SourceIn, PrDrawBlendMode_Normal},
    {"source-out", PrDrawComposite_SourceOut, PrDrawBlendMode_Normal},
    {"source-atop", PrDrawComposite_SourceAtop, PrDrawBlendMode_Normal},
    {"destination-over", PrDrawComposite_DestinationOver, PrDrawBlendMode_Normal},
    {"destination-in", PrDrawComposite_DestinationIn, PrDrawBlendMode_Normal},
    {"destination-out", PrDrawComposite_DestinationOut, PrDrawBlendMode_Normal},
    {"destination-atop", PrDrawComposite_DestinationAtop, PrDrawBlendMode_Normal},
    {"lighter", PrDrawComposite_PlusLighter, PrDrawBlendMode_Normal},
    {"copy", PrDrawComposite_Copy, PrDrawBlendMode_Normal},
    {"xor", PrDrawComposite_Xor, PrDrawBlendMode_Normal},
    {"clear", PrDrawComposite_Clear, PrDrawBlendMode_Normal},
    {"normal", PrDrawComposite_SourceOver, PrDrawBlendMode_Normal},
    {"multiply", PrDrawComposite_SourceOver, PrDrawBlendMode_Multiply},
    {"screen", PrDrawComposite_SourceOver, PrDrawBlendMode_Screen},
    {"overlay", PrDrawComposite_SourceOver, PrDrawBlendMode_Overlay},
    {"darken", PrDrawComposite_SourceOver, PrDrawBlendMode_Darken},
    {"lighten", PrDrawComposite_SourceOver, PrDrawBlendMode_Lighten},
    {"color-dodge", PrDrawComposite_SourceOver, PrDrawBlendMode_ColorDodge},
    {"color-burn", PrDrawComposite_SourceOver, PrDrawBlendMode_ColorBurn},
    {"hard-light", PrDrawComposite_SourceOver, PrDrawBlendMode_HardLight},
    {"soft-light", PrDrawComposite_SourceOver, PrDrawBlendMode_SoftLight},
    {"difference", PrDrawComposite_SourceOver, PrDrawBlendMode_Difference},
    {"exclusion", PrDrawComposite_SourceOver, PrDrawBlendMode_Exclusion},
    {"hue", PrDrawComposite_SourceOver, PrDrawBlendMode_Hue},
    {"saturation", PrDrawComposite_SourceOver, PrDrawBlendMode_Saturation},
    {"color", PrDrawComposite_SourceOver, PrDrawBlendMode_Color},
    {"luminosity", PrDrawComposite_SourceOver, PrDrawBlendMode_Luminosity},
};

// The serializations of the initial fill and stroke styles, of the initial shadow color and filter, and of the initial
// language, font and spacing.
constexpr PrStringView kBlack = "#000000";
constexpr PrStringView kTransparentBlack = "rgba(0, 0, 0, 0)";
constexpr PrStringView kNone = "none";
constexpr PrStringView kInherit = "inherit";
constexpr PrStringView kDefaultFont = "10px sans-serif";
constexpr PrStringView kZeroLength = "0px";

// The width of each value of fontStretch, as a percentage of the normal width (css-fonts-4/#font-width-prop).
constexpr float kStretchWidths[] = {50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200};

// A spacing of the text styles (html/#dom-context-2d-letterspacing): a CSS <length> as its number and unit, and its
// serialization.
struct CanvasSpacing {
    double value;
    PrCssUnit unit;
    PrString text;
};

// A gradient (html/#canvasgradient). Its kind decides which geometry it uses: the line from `start` to `end`, the
// circles around them, or the angle around `start`.
struct CanvasGradient {
    uint32_t references;
    PrDrawBrushKind kind;
    PrDVec2 start;
    PrDVec2 end;
    double startRadius;
    double endRadius;
    double angle;

    // The stops in the order of their offsets. A stop comes after the stops that were added before it at its offset.
    Array<PrDrawColorStop> stops;
};

// A pattern (html/#canvaspattern): an image, repeated as `repetition` says, under its own transform.
struct CanvasPattern {
    uint32_t references;
    const PrDrawImage* image;                   // the pattern holds a reference
    PrVec2 extent;                              // the image's extent in CSS pixels
    PrCanvasRepetition repetition;
    PrDAffine2 transform;
};

void gradientRelease(CanvasGradient* PR_NULLABLE gradient) {
    if (gradient == nullptr || --gradient->references != 0)
        return;
    gradient->stops.reset();
    prMemoryFree(gradient);
}

void patternRelease(CanvasPattern* PR_NULLABLE pattern) {
    if (pattern == nullptr || --pattern->references != 0)
        return;
    prDrawImageRelease(pattern->image);
    prMemoryFree(pattern);
}

// A fill or stroke style (html/#dom-context-2d-fillstyle). Its kind decides which fields it uses.
struct CanvasStyleValue {
    PrCanvasStyleKind kind;
    PrColor color;                              // Color: straight alpha, extended sRGB
    PrString text;                              // Color: the serialization
    CanvasGradient* PR_NULLABLE gradient;       // Gradient: the style holds a reference
    CanvasPattern* PR_NULLABLE pattern;         // Pattern: the style holds a reference
};

// An area of the clipping region: a path in the bitmap's space, filled by its rule. The id names the area for the life
// of the context, so that a saved state shares it with the state that it was saved from.
struct CanvasClip {
    PrPath* path;
    uint64_t id;
    PrPathFillRule rule;
};

// The drawing state (html/#drawing-state).
struct CanvasState {
    PrDAffine2 transform;
    Array<CanvasClip> clips;                    // the clipping region is their intersection
    CanvasStyleValue styles[2];                 // by PrCanvasStyleRole
    double globalAlpha;
    uint32_t operation;                         // in kOperations
    bool smoothing;
    PrCanvasImageSmoothingQuality smoothingQuality;

    // The line styles.
    double lineWidth;
    double miterLimit;
    double dashOffset;
    Array<double> dashes;                       // an even count
    PrCanvasLineCap lineCap;
    PrCanvasLineJoin lineJoin;

    // The shadow styles. The color is straight alpha, in extended sRGB.
    PrDVec2 shadowOffset;
    double shadowBlur;
    PrColor shadowColor;
    PrString shadowText;

    // The current filter, as it was set, and its items resolved, which are none for "none".
    PrString filterText;
    Array<PrLayoutFilter> filters;

    // The text styles. The font holds a reference.
    CanvasFont* font;
    PrString lang;
    CanvasSpacing letterSpacing;
    CanvasSpacing wordSpacing;
    PrCanvasTextAlign textAlign;
    PrCanvasTextBaseline textBaseline;
    PrCanvasDirection direction;
    PrCanvasFontKerning fontKerning;
    PrCanvasFontStretch fontStretch;
    PrCanvasFontVariantCaps fontVariantCaps;
    PrCanvasTextRendering textRendering;
};

// A context's bitmap as the renderers that draw it see it: a texture on a device, or pixels in memory, which holds what
// the context drew up to the revision `drawn` (PrDrawGroupLayer). The context and each snapshot that shows it hold a
// reference, so a list that draws it after the context is gone draws into storage that is still there.
struct CanvasBitmap {
    Atomic<uint32_t> references;
    PrUVec2 extent;
    PrPixelFormat format;
    uint64_t drawn;                             // read and raised atomically (PrDrawGroupLayer)
    PrGpuDevice* PR_NULLABLE device;
    PrGpuTexture texture;                       // on a device
    PrDrawTextureId id;                         // the texture's id, or the id of `cpu`
    uint8_t* PR_NULLABLE pixels;                // in memory
    PrDrawCpuTexture cpu;
};

// Drawing that is recorded and waits to be drawn into the bitmap: a frozen list, and its revision.
struct CanvasSegment {
    PrDrawList* list;
    uint64_t revision;
};

// What a context shows as of a presentation: its bitmap, and the segments that the bitmap may not hold yet. It is
// immutable, and the painters and lists that draw it hold a reference to it.
struct CanvasShown {
    Atomic<uint32_t> references;
    CanvasBitmap* bitmap;
    Array<CanvasSegment> segments;
    PrDrawSampling sampling;
};

struct CanvasContext {
    PrCanvasSettings settings;
    const PrDomElement* PR_NULLABLE element;

    // The owner's callback, and whether it was called since the last snapshot.
    PrCanvasChangedFn PR_NULLABLE changedFn;
    void* PR_NULLABLE changedContext;
    bool notified;

    // The bitmap: premultiplied extended sRGB in `format`, or no pixels for a bitmap of no area or past
    // kPrCanvasBitmapMaxSize. `bitmap` holds it. In memory, `pixels` are the bitmap's. On a device, the texture holds
    // it, and `pixels` hold a copy of it while `pixelsCurrent`.
    PrUVec2 extent;
    PrPixelFormat format;
    uint8_t* PR_NULLABLE pixels;
    bool originClean;
    uint64_t version;                           // raised by every change to what the bitmap shows
    const PrDrawImage* PR_NULLABLE snapshot;    // the pixels as an image, until they next change

    CanvasBitmap* PR_NULLABLE bitmap;
    PrGpuDevice* PR_NULLABLE device;
    bool pixelsCurrent;

    // The context's own renderer, which draws the segments into the bitmap when its pixels are read or written: on
    // `device`, or in memory without one. It is made on first use. On a device, the frame that it prepares holds the
    // list that it drew until the next draw.
    PrDrawRenderer* PR_NULLABLE renderer;
    PrDrawFrame* PR_NULLABLE frame;
    PrDrawList* PR_NULLABLE drawnList;

    // The drawing that is recorded and not yet a segment, the clips that it holds open (the ids of the areas of a
    // clipping region, outermost first), the segments that the bitmap may not hold yet, and the last revision given.
    PrDrawList* pending;
    bool recorded;                              // `pending` holds a draw
    Array<CanvasSegment> segments;
    uint64_t revision;
    Array<uint64_t> openClips;
    uint64_t nextClipId;

    CanvasState state;
    Array<CanvasState> saved;                   // the drawing state stack, oldest first
    CanvasPath path;                            // the current default path, in the bitmap's space

    Array<float> dashes;                        // scratch: the dash list as floats

    // The owner's source of what text is set with, the initial font, which holds a reference, and text set for the last
    // call that drew or measured it.
    PrCanvasTextSourceFn PR_NULLABLE textSourceFn;
    void* PR_NULLABLE textSourceContext;
    CanvasFont* defaultFont;
    CanvasText text;
};

PrAffine2 affineOf(PrDAffine2 transform) {
    return prAffine2FromDAffine2(transform);
}

bool affineIsFinite(PrDAffine2 t) {
    return canvasIsFinite(t.a, t.b, t.c, t.d, t.e, t.f);
}

bool invertible(PrDAffine2 transform) {
    PrDAffine2 inverse{};
    return prDAffine2Invert(transform, &inverse);
}

// Returns the extended sRGB of a used color, with straight alpha. A missing component is zero.
PrColor srgbOf(const PrCssUsedColor& color) {
    double in[3];
    for (uint32_t i = 0; i < 3; ++i)
        in[i] = (color.missing & (1u << i)) != 0 ? 0 : color.components[i];
    double out[3] = {in[0], in[1], in[2]};
    if (color.space != PrColorSpace_Srgb)
        prColorConvert(color.space, PrColorSpace_Srgb, in, out);
    double alpha = (color.missing & 8u) != 0 ? 0 : color.alpha;
    return PrColor{float(out[0]), float(out[1]), float(out[2]), float(alpha)};
}

// Parses `text` as a CSS color (html/#parsed-as-a-css-color-value), and writes it and, if `serialization` is not null,
// its serialization. Returns false if it does not parse.
bool parseColor(PrStringView text, const PrDomElement* PR_NULLABLE element, PrColor* color,
                PrString* PR_NULLABLE serialization) {
    PrCssUsedColor parsed{};
    if (prCssParseColor(text, element, &parsed) != PrStatus_Ok)
        return false;
    *color = srgbOf(parsed);
    if (serialization != nullptr)
        *serialization = prCssUsedColorSerialize(&parsed, PrCssColorSerialization_HtmlCompatible);
    return true;
}

// --- Styles ---

void styleReset(CanvasStyleValue* style) {
    prStringReset(&style->text);
    gradientRelease(style->gradient);
    patternRelease(style->pattern);
    *style = CanvasStyleValue{};
}

void styleCopy(CanvasStyleValue* out, const CanvasStyleValue& from) {
    *out = from;
    out->text = prStringClone(&from.text);
    if (from.gradient != nullptr)
        ++from.gradient->references;
    if (from.pattern != nullptr)
        ++from.pattern->references;
}

// --- The drawing state ---

void stateInit(CanvasState* state, CanvasFont* font) {
    *state = CanvasState{};
    state->transform = prDAffine2Identity();
    for (CanvasStyleValue& style : state->styles)
        style = CanvasStyleValue{.kind = PrCanvasStyleKind_Color, .color = PrColor{0, 0, 0, 1},
                            .text = prStringExternal(kBlack)};
    state->globalAlpha = 1;
    state->smoothing = true;
    state->smoothingQuality = PrCanvasImageSmoothingQuality_Low;
    state->lineWidth = 1;
    state->miterLimit = 10;
    state->lineCap = PrCanvasLineCap_Butt;
    state->lineJoin = PrCanvasLineJoin_Miter;
    state->shadowText = prStringExternal(kTransparentBlack);
    state->filterText = prStringExternal(kNone);
    canvasFontRetain(font);
    state->font = font;
    state->lang = prStringExternal(kInherit);
    state->letterSpacing = CanvasSpacing{.unit = PrCssUnit_Px, .text = prStringExternal(kZeroLength)};
    state->wordSpacing = CanvasSpacing{.unit = PrCssUnit_Px, .text = prStringExternal(kZeroLength)};
    state->textAlign = PrCanvasTextAlign_Start;
    state->textBaseline = PrCanvasTextBaseline_Alphabetic;
    state->direction = PrCanvasDirection_Inherit;
    state->fontKerning = PrCanvasFontKerning_Auto;
    state->fontStretch = PrCanvasFontStretch_Normal;
    state->fontVariantCaps = PrCanvasFontVariantCaps_Normal;
    state->textRendering = PrCanvasTextRendering_Auto;
}

void stateReset(CanvasState* state) {
    for (uint32_t i = 0; i < state->clips.count; ++i)
        prPathDestroy(state->clips.data[i].path);
    state->clips.reset();
    for (CanvasStyleValue& style : state->styles)
        styleReset(&style);
    state->dashes.reset();
    prStringReset(&state->shadowText);
    prStringReset(&state->filterText);
    state->filters.reset();
    canvasFontRelease(state->font);
    prStringReset(&state->lang);
    prStringReset(&state->letterSpacing.text);
    prStringReset(&state->wordSpacing.text);
}

void stateCopy(CanvasState* out, const CanvasState& from) {
    memCopy(out, &from, sizeof(CanvasState));
    out->clips = {};
    out->clips.reserve(from.clips.count);
    for (uint32_t i = 0; i < from.clips.count; ++i) {
        const CanvasClip& clip = from.clips.data[i];
        out->clips.push(CanvasClip{prPathClone(clip.path), clip.id, clip.rule});
    }
    for (uint32_t i = 0; i < 2; ++i)
        styleCopy(&out->styles[i], from.styles[i]);
    out->dashes = {};
    if (from.dashes.count != 0)
        out->dashes.append(from.dashes.data, from.dashes.count);
    out->shadowText = prStringClone(&from.shadowText);
    out->filterText = prStringClone(&from.filterText);
    out->filters = {};
    if (from.filters.count != 0)
        out->filters.append(from.filters.data, from.filters.count);
    canvasFontRetain(from.font);
    out->lang = prStringClone(&from.lang);
    out->letterSpacing.text = prStringClone(&from.letterSpacing.text);
    out->wordSpacing.text = prStringClone(&from.wordSpacing.text);
}

// --- Text ---

// Parses `text` as a CSS <length> of spacing (html/#dom-context-2d-letterspacing). Returns false if it is not one.
bool parseSpacing(PrStringView text, CanvasSpacing* out) {
    PrCssValue* value = prCssValueCreate();
    bool parsed = prCssValueParseNumeric(text, value) == PrStatus_Ok && prCssValueSize(value) == 1;
    PrCssValueFields fields = parsed ? prCssValueFields(value, 0) : PrCssValueFields{};
    PrCssNumericType type{};
    bool length = parsed && fields.kind == PrCssValueKind_Dimension
                  && prCssNumericTypeFromUnit(fields.number.unit, &type);
    for (uint32_t i = 0; length && i < PrCssNumericBaseType_Count; ++i)
        length = type.exponents[i] == (i == PrCssNumericBaseType_Length ? 1 : 0);
    if (length)
        *out = CanvasSpacing{fields.number.value, fields.number.unit, prCssValueSerialize(value, 0, 1)};
    prCssValueDestroy(value);
    return length;
}

// Returns a spacing in px for text of `size` px. A unit relative to the font is relative to that size, with the
// fallbacks of css-values-4/#font-relative-lengths, and the root's font is the initial 16px. A unit relative to the
// viewport has no viewport to measure, and is zero.
double spacingPx(const CanvasSpacing& spacing, double size) {
    double unit = 0;
    switch (spacing.unit) {
        case PrCssUnit_Px: unit = 1; break;
        case PrCssUnit_Cm: unit = 96 / 2.54; break;
        case PrCssUnit_Mm: unit = 96 / 25.4; break;
        case PrCssUnit_Q: unit = 96 / 101.6; break;
        case PrCssUnit_In: unit = 96; break;
        case PrCssUnit_Pc: unit = 16; break;
        case PrCssUnit_Pt: unit = 96.0 / 72; break;
        case PrCssUnit_Em: unit = size; break;
        case PrCssUnit_Rem: unit = 16; break;
        case PrCssUnit_Ex: unit = size / 2; break;
        case PrCssUnit_Cap: unit = size * 0.7; break;
        case PrCssUnit_Ch: unit = size / 2; break;
        case PrCssUnit_Ic: unit = size; break;
        case PrCssUnit_Lh: unit = size * 1.2; break;
        default: break;
    }
    return spacing.value * unit;
}

// --- The bitmap ---

// Returns true if the bitmap holds each channel in 8 bits, and false if it holds half floats.
bool holdsBytes(const CanvasContext* c) {
    return c->format == PrPixelFormat_Rgba8;
}

size_t strideOf(const CanvasContext* c) {
    return size_t(c->extent.x) * prPixelFormatSize(c->format);
}

PrPixels pixelsOf(const CanvasContext* c) {
    return PrPixels{c->pixels, strideOf(c), c->extent};
}

// Records that what the bitmap shows may have changed, and tells the owner the first time after a snapshot.
void touched(CanvasContext* c) {
    ++c->version;
    if (c->notified || c->changedFn == nullptr)
        return;
    c->notified = true;
    c->changedFn(c->changedContext);
}

// Records that the bitmap's pixels changed (touched), which makes the snapshot stale.
void changed(CanvasContext* c) {
    touched(c);
    prDrawImageRelease(c->snapshot);
    c->snapshot = nullptr;
}

// Makes every pixel of a bitmap without alpha opaque, as if what was drawn had been drawn over opaque black.
void makeOpaque(CanvasContext* c) {
    uint32_t count = c->extent.x * c->extent.y;
    if (holdsBytes(c)) {
        for (uint32_t i = 0; i < count; ++i)
            c->pixels[i * 4 + 3] = 255;
        return;
    }
    PrFloat16* halves = reinterpret_cast<PrFloat16*>(c->pixels);
    for (uint32_t i = 0; i < count; ++i)
        halves[i * 4 + 3] = prFloat16FromFloat(1);
}

// Returns the color that a bitmap starts as: transparent black, or opaque black for a bitmap without alpha.
PrColor clearColorOf(const CanvasContext* c) {
    return PrColor{0, 0, 0, c->settings.alpha ? 0.0f : 1.0f};
}

void bitmapRelease(CanvasBitmap* PR_NULLABLE bitmap) {
    if (bitmap == nullptr || bitmap->references.fetchSub(1) != 1)
        return;
    if (bitmap->device != nullptr)
        prGpuTextureDestroy(bitmap->device, bitmap->texture);
    prMemoryFree(bitmap->pixels);
    prMemoryFree(bitmap);
}

void segmentsReset(Array<CanvasSegment>* segments) {
    for (uint32_t i = 0; i < segments->count; ++i)
        prDrawListRelease(segments->data[i].list);
    segments->reset();
}

void shownRelease(void* PR_NULLABLE data) {
    CanvasShown* shown = static_cast<CanvasShown*>(data);
    if (shown == nullptr || shown->references.fetchSub(1) != 1)
        return;
    bitmapRelease(shown->bitmap);
    segmentsReset(&shown->segments);
    prMemoryFree(shown);
}

void shownRetain(void* PR_NULLABLE data) {
    static_cast<CanvasShown*>(data)->references.fetchAdd(1);
}

// Closes the clips that the pending drawing holds open.
void closeClips(CanvasContext* c) {
    for (uint32_t i = 0; i < c->openClips.count; ++i)
        prDrawListPopClip(c->pending);
    c->openClips.clear();
}

// Makes the pending drawing a segment of the next revision. A bitmap without alpha is drawn over opaque black at the
// end of each, so that what it holds stays opaque.
void commit(CanvasContext* c) {
    if (!c->recorded)
        return;
    closeClips(c);
    if (!c->settings.alpha) {
        PrRect bitmap{0, 0, float(c->extent.x), float(c->extent.y)};
        PrDrawPaint black{.color = PrColor{0, 0, 0, 1}, .composite = PrDrawComposite_DestinationOver};
        prDrawListRect(c->pending, bitmap, black);
    }
    prDrawListFreeze(c->pending);
    c->segments.push(CanvasSegment{c->pending, ++c->revision});
    c->pending = prDrawListCreate();
    c->recorded = false;
    c->pixelsCurrent = false;
}

// Drops the segments that the bitmap holds.
void dropDrawn(CanvasContext* c) {
    if (c->bitmap == nullptr)
        return;
    // Renderers on other threads raise the revision as they claim segments (PrDrawGroupLayer).
    uint64_t drawn = reinterpret_cast<const Atomic<uint64_t>*>(&c->bitmap->drawn)->load();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < c->segments.count; ++i) {
        if (c->segments.data[i].revision <= drawn)
            prDrawListRelease(c->segments.data[i].list);
        else
            c->segments.data[kept++] = c->segments.data[i];
    }
    c->segments.count = kept;
}

// Returns what the context shows now, with one reference: the bitmap and the segments that it may not hold.
CanvasShown* show(CanvasContext* c, PrDrawSampling sampling) {
    commit(c);
    dropDrawn(c);
    CanvasShown* shown = allocZeroed<CanvasShown>();
    shown->references.storeRelaxed(1);
    shown->bitmap = c->bitmap;
    shown->bitmap->references.fetchAdd(1);
    shown->sampling = sampling;
    for (uint32_t i = 0; i < c->segments.count; ++i) {
        prDrawListRetain(c->segments.data[i].list);
        shown->segments.push(c->segments.data[i]);
    }
    return shown;
}

// Draws what `shown` shows over the rectangle from the origin to `extent`: the segments drawn into the bitmap, each
// once, then the bitmap (PrLayoutPaintFn). The list holds `shown`.
void paintShown(void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list) {
    CanvasShown* shown = static_cast<CanvasShown*>(context);
    CanvasBitmap* bitmap = shown->bitmap;
    PrVec2 size{float(bitmap->extent.x), float(bitmap->extent.y)};
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{extent.x / size.x, extent.y / size.y}));
    uint64_t newest = shown->segments.count != 0 ? shown->segments.last().revision : 0;
    PrDrawGroupLayer layer{.texture = bitmap->id, .extent = bitmap->extent, .drawn = &bitmap->drawn,
                           .revision = newest, .sampling = shown->sampling};
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{0, 0, size.x, size.y};
    group.layer = &layer;
    prDrawListPushGroup(list, &group);
    for (uint32_t i = 0; i < shown->segments.count; ++i) {
        PrDrawGroup segment = prDrawGroupDefault();
        segment.revision = shown->segments.data[i].revision;
        prDrawListPushGroup(list, &segment);
        prDrawListEmbed(list, shown->segments.data[i].list, PrDVec2{0, 0});
        prDrawListPopGroup(list);
    }
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    shownRetain(shown);
    prDrawListHold(list, shownRelease, shown);
}

// Draws the segments that the bitmap may not hold into it with the context's own renderer, so that the bitmap holds
// everything that the context drew. In memory, the pixels hold it when the call returns. On a device, the call submits
// the commands that draw it, and work submitted after them sees it.
void flush(CanvasContext* c) {
    commit(c);
    dropDrawn(c);
    if (c->segments.count == 0 || c->bitmap == nullptr)
        return;
    if (c->renderer == nullptr) {
        PrDrawRendererBackend backend =
            c->device != nullptr ? PrDrawRendererBackend_Default : PrDrawRendererBackend_Cpu;
        if (prDrawRendererCreate(backend, c->device, nullptr, &c->renderer) != PrStatus_Ok)
            return;
    }
    CanvasShown* shown = show(c, PrDrawSampling_Nearest);
    PrDrawList* list = prDrawListCreate();
    paintShown(shown, PrVec2{1, 1}, list);
    prDrawListFreeze(list);
    shownRelease(shown);
    // The list draws into the bitmap as it is prepared, and renders over a pixel that nothing shows.
    if (c->device == nullptr) {
        uint8_t pixel[8] = {};
        prDrawRendererRenderPixels(c->renderer, list, PrPixels{pixel, sizeof(pixel), {1, 1}}, PrPixelFormat_RgbaF16);
        prDrawListRelease(list);
    } else {
        if (c->frame == nullptr)
            c->frame = prDrawFrameCreate(c->renderer);
        PrGpuCommands* commands = nullptr;
        if (prGpuCommandsBegin(c->device, nullptr, &commands)) {
            PrDrawTarget target{.extent = PrUVec2{1, 1}, .format = PrPixelFormat_RgbaF16, .commands = commands};
            prDrawFramePrepare(c->frame, list, &target);
            prGpuCommandsSubmit(commands);
        }
        prDrawListRelease(c->drawnList);
        c->drawnList = list;
    }
    dropDrawn(c);
    prDrawImageRelease(c->snapshot);
    c->snapshot = nullptr;
}

// Draws what the context recorded into the bitmap, and brings the pixels up to date with a bitmap on a device, which
// waits for the device.
void readPixels(CanvasContext* c) {
    flush(c);
    if (c->device == nullptr || c->pixelsCurrent || c->bitmap == nullptr || c->bitmap->texture.id == 0)
        return;
    prGpuTextureRead(c->device, c->bitmap->texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, pixelsOf(c));
    c->pixelsCurrent = true;
}

// Writes the bitmap's pixels to a bitmap on a device, once they have changed in memory. The write is an upload, which
// waits for the work that reads the texture.
void writePixels(CanvasContext* c) {
    if (c->bitmap == nullptr || c->bitmap->texture.id == 0)
        return;
    prGpuTextureWrite(c->device, c->bitmap->texture, PrIVec2{0, 0}, prPixelsView(pixelsOf(c)));
}

// Fills the bitmap with transparent black, or with opaque black for a bitmap without alpha: drawing that the bitmap
// does not hold yet is dropped, and a segment clears the bitmap whole.
void clearBitmap(CanvasContext* c) {
    closeClips(c);
    prDrawListClear(c->pending);
    segmentsReset(&c->segments);
    if (c->pixels != nullptr) {
        memZero(c->pixels, strideOf(c) * c->extent.y);
        if (!c->settings.alpha)
            makeOpaque(c);
        prDrawListClearRect(c->pending, PrRect{0, 0, float(c->extent.x), float(c->extent.y)}, clearColorOf(c));
        c->recorded = true;
        commit(c);
        c->pixelsCurrent = true;
    }
    changed(c);
}

// Allocates the bitmap at `extent`, transparent or opaque black, and frees the one before. On a device, a bitmap that
// the device cannot make has no pixels.
void allocateBitmap(CanvasContext* c, PrUVec2 extent) {
    if (c->device != nullptr)
        prMemoryFree(c->pixels);
    c->pixels = nullptr;
    bitmapRelease(c->bitmap);
    c->bitmap = nullptr;
    c->extent = extent;
    if (extent.x != 0 && extent.y != 0 && extent.x <= kPrCanvasBitmapMaxSize && extent.y <= kPrCanvasBitmapMaxSize) {
        CanvasBitmap* bitmap = allocZeroed<CanvasBitmap>();
        bitmap->references.storeRelaxed(1);
        bitmap->extent = extent;
        bitmap->format = c->format;
        c->pixels = static_cast<uint8_t*>(prMemoryAlloc(strideOf(c) * extent.y));
        if (c->device == nullptr) {
            bitmap->pixels = c->pixels;
            bitmap->cpu = PrDrawCpuTexture{pixelsOf(c), c->format};
            bitmap->id = prDrawCpuTexture(&bitmap->cpu);
            c->bitmap = bitmap;
        } else {
            PrGpuTextureDesc desc = prGpuTextureDesc2d(
                prGpuFormatFromPixelFormat(c->format), extent,
                PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
            desc.name = "canvas";
            if (prGpuTextureCreate(c->device, &desc, &bitmap->texture) == PrStatus_Ok) {
                bitmap->device = c->device;
                bitmap->id = prDrawGpuTexture(bitmap->texture);
                c->bitmap = bitmap;
            } else {
                bitmapRelease(bitmap);
                prMemoryFree(c->pixels);
                c->pixels = nullptr;
            }
        }
    }
    clearBitmap(c);
}

// Discards the pending drawing.
void discard(CanvasContext* c) {
    closeClips(c);
    prDrawListClear(c->pending);
    c->recorded = false;
}

// Brings the clips that the pending drawing holds open in line with the clipping region: the areas that both share,
// outermost first, stay open, and the rest of the region's are pushed.
void syncClips(CanvasContext* c) {
    const Array<CanvasClip>& clips = c->state.clips;
    uint32_t shared = 0;
    while (shared < c->openClips.count && shared < clips.count && c->openClips.data[shared] == clips.data[shared].id)
        ++shared;
    while (c->openClips.count > shared) {
        prDrawListPopClip(c->pending);
        c->openClips.pop();
    }
    for (uint32_t i = shared; i < clips.count; ++i) {
        prDrawListPushClipPath(c->pending, clips.data[i].path, clips.data[i].rule, PrDrawEdges_Antialiased);
        c->openClips.push(clips.data[i].id);
    }
}

// Reads the pixel at `index` of the bitmap as straight-alpha extended sRGB.
PrColor readPixel(const CanvasContext* c, size_t index) {
    PrColor p{};
    if (holdsBytes(c)) {
        const uint8_t* q = c->pixels + index * 4;
        p = PrColor{q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, q[3] / 255.0f};
    } else {
        const PrFloat16* q = reinterpret_cast<const PrFloat16*>(c->pixels) + index * 4;
        p = PrColor{prFloat16ToFloat(q[0]), prFloat16ToFloat(q[1]), prFloat16ToFloat(q[2]), prFloat16ToFloat(q[3])};
    }
    if (p.a <= 0)
        return PrColor{0, 0, 0, 0};
    return PrColor{p.r / p.a, p.g / p.a, p.b / p.a, p.a};
}

// Writes `color`, straight-alpha extended sRGB, to the pixel at `index` of the bitmap.
void writePixel(CanvasContext* c, size_t index, PrColor color) {
    float a = c->settings.alpha ? clampF32(color.a, 0, 1) : 1;
    PrColor p{color.r * a, color.g * a, color.b * a, a};
    if (holdsBytes(c)) {
        uint8_t* q = c->pixels + index * 4;
        const float channels[] = {p.r, p.g, p.b, p.a};
        for (uint32_t i = 0; i < 4; ++i)
            q[i] = uint8_t(roundF32(clampF32(channels[i], 0, 1) * 255));
        return;
    }
    PrFloat16* q = reinterpret_cast<PrFloat16*>(c->pixels) + index * 4;
    q[0] = prFloat16FromFloat(p.r);
    q[1] = prFloat16FromFloat(p.g);
    q[2] = prFloat16FromFloat(p.b);
    q[3] = prFloat16FromFloat(p.a);
}

// Reads the pixel at `index` of ImageData pixels of `format` as straight alpha in the data's space.
PrColor readData(const void* pixels, PrCanvasPixelFormat format, size_t index) {
    if (format == PrCanvasPixelFormat_RgbaUnorm8) {
        const uint8_t* q = static_cast<const uint8_t*>(pixels) + index * 4;
        return PrColor{q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f, q[3] / 255.0f};
    }
    const PrFloat16* q = static_cast<const PrFloat16*>(pixels) + index * 4;
    return PrColor{prFloat16ToFloat(q[0]), prFloat16ToFloat(q[1]), prFloat16ToFloat(q[2]), prFloat16ToFloat(q[3])};
}

void writeData(void* pixels, PrCanvasPixelFormat format, size_t index, PrColor color) {
    if (format == PrCanvasPixelFormat_RgbaUnorm8) {
        uint8_t* q = static_cast<uint8_t*>(pixels) + index * 4;
        const float channels[] = {color.r, color.g, color.b, color.a};
        for (uint32_t i = 0; i < 4; ++i)
            q[i] = uint8_t(roundF32(clampF32(channels[i], 0, 1) * 255));
        return;
    }
    PrFloat16* q = static_cast<PrFloat16*>(pixels) + index * 4;
    q[0] = prFloat16FromFloat(color.r);
    q[1] = prFloat16FromFloat(color.g);
    q[2] = prFloat16FromFloat(color.b);
    q[3] = prFloat16FromFloat(color.a);
}

// Returns `color` with its components converted from `from` to `to`, sRGB or Display P3.
PrColor convertColor(PrColor color, PrColorSpace from, PrColorSpace to) {
    if (from == to)
        return color;
    double in[3] = {double(color.r), double(color.g), double(color.b)};
    double out[3];
    prColorConvert(from, to, in, out);
    return PrColor{float(out[0]), float(out[1]), float(out[2]), color.a};
}

// --- Paint ---

// Returns the sampling that images are drawn with (html/#dom-context-2d-imagesmoothingenabled).
PrDrawSampling samplingOf(const CanvasState& state) {
    if (!state.smoothing)
        return PrDrawSampling_Nearest;
    switch (state.smoothingQuality) {
        case PrCanvasImageSmoothingQuality_Medium: return PrDrawSampling_Mipmap;
        case PrCanvasImageSmoothingQuality_High: return PrDrawSampling_Cubic;
        default: return PrDrawSampling_Linear;
    }
}

// Writes the brush of a gradient, from gradient space into the space of the draw through `transform`. Returns false if
// the gradient paints nothing: it has no stops, its geometry is degenerate (html/#dom-context-2d-createlineargradient),
// or it is radial under a transform that collapses the plane.
bool gradientBrush(const CanvasGradient& g, PrDAffine2 transform, PrDrawBrush* out) {
    if (g.stops.count == 0)
        return false;
    PrVec2 start{float(g.start.x), float(g.start.y)};
    PrVec2 end{float(g.end.x), float(g.end.y)};
    switch (g.kind) {
        case PrDrawBrushKind_Linear:
            if (g.start.x == g.end.x && g.start.y == g.end.y)
                return false;
            *out = prDrawBrushLinear(start, end, g.stops.data, g.stops.count);
            break;
        case PrDrawBrushKind_Radial:
            if ((g.start.x == g.end.x && g.start.y == g.end.y && g.startRadius == g.endRadius)
                || !invertible(transform))
                return false;
            *out = prDrawBrushRadial(start, float(g.startRadius), end, float(g.endRadius), g.stops.data,
                                     g.stops.count);
            break;
        default:
            if (!invertible(transform))
                return false;
            *out = prDrawBrushConic(start, float(g.angle), g.stops.data, g.stops.count);
            break;
    }
    out->interpolationAlpha = PrDrawAlpha_Straight;
    out->transform = affineOf(transform);
    return true;
}

// Writes the brush of a pattern, from the pattern's space through its own transform and then `transform`. Returns false
// if the pattern is under a transform that collapses the plane, where it paints nothing.
bool patternBrush(const CanvasPattern& p, PrDAffine2 transform, PrDrawSampling sampling, PrDrawBrush* out) {
    PrDAffine2 mapped = prDAffine2Concat(p.transform, transform);
    if (!invertible(mapped))
        return false;
    PrUVec2 pixels = prDrawImageExtent(p.image);
    PrDrawSource source = prDrawSourceImage(p.image, PrRect{0, 0, float(pixels.x), float(pixels.y)});
    *out = prDrawBrushImage(source, PrRect{0, 0, p.extent.x, p.extent.y}, sampling);
    bool repeatX = p.repetition == PrCanvasRepetition_Repeat || p.repetition == PrCanvasRepetition_RepeatX;
    bool repeatY = p.repetition == PrCanvasRepetition_Repeat || p.repetition == PrCanvasRepetition_RepeatY;
    out->image.tiles.x = repeatX ? PrDrawExtend_Repeat : PrDrawExtend_None;
    out->image.tiles.y = repeatY ? PrDrawExtend_Repeat : PrDrawExtend_None;
    out->transform = affineOf(mapped);
    return true;
}

// The paint of a draw: a color, or a brush that `paint` points to.
struct CanvasPaint {
    PrDrawPaint paint;
    PrDrawBrush brush;
};

// Writes the paint of style `role` at the global alpha (html/#drawing-model), with brush space mapped through
// `brushTransform` into the space of the draw. A style that paints nothing paints transparent black.
void paintOf(const CanvasContext* c, PrCanvasStyleRole role, PrDAffine2 brushTransform, CanvasPaint* out) {
    const CanvasState& s = c->state;
    const CanvasStyleValue& style = s.styles[role];
    *out = CanvasPaint{};
    bool brushed = false;
    if (style.kind == PrCanvasStyleKind_Gradient)
        brushed = gradientBrush(*style.gradient, brushTransform, &out->brush);
    else if (style.kind == PrCanvasStyleKind_Pattern)
        brushed = patternBrush(*style.pattern, brushTransform, samplingOf(s), &out->brush);
    if (brushed) {
        out->brush.opacity = float(s.globalAlpha);
        out->paint.brush = &out->brush;
    } else if (style.kind == PrCanvasStyleKind_Color) {
        PrColor color = style.color;
        color.a *= float(s.globalAlpha);
        out->paint.color = color;
    }
}

// Returns the stroke of the line styles, in the space of the draw. Its dashes are valid until the next call.
PrDrawStroke strokeOf(CanvasContext* c) {
    const CanvasState& s = c->state;
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = float(s.lineWidth);
    stroke.shape.miterLimit = float(s.miterLimit);
    constexpr PrPathJoin kJoins[] = {PrPathJoin_Round, PrPathJoin_Bevel, PrPathJoin_Miter};
    constexpr PrPathCap kCaps[] = {PrPathCap_Butt, PrPathCap_Round, PrPathCap_Square};
    stroke.shape.join = kJoins[s.lineJoin];
    stroke.shape.startCap = kCaps[s.lineCap];
    stroke.shape.endCap = kCaps[s.lineCap];
    stroke.shape.dashCap = kCaps[s.lineCap];
    c->dashes.clear();
    for (uint32_t i = 0; i < s.dashes.count; ++i)
        c->dashes.push(float(s.dashes.data[i]));
    stroke.shape.dashes = c->dashes.count != 0 ? c->dashes.data : nullptr;
    stroke.shape.dashCount = c->dashes.count;
    stroke.shape.dashOffset = float(s.dashOffset);
    return stroke;
}

// Returns true if shadows are drawn (html/#when-shadows-are-drawn).
bool drawsShadow(const CanvasState& s) {
    return s.shadowColor.a > 0 && (s.shadowBlur > 0 || s.shadowOffset.x != 0 || s.shadowOffset.y != 0);
}

// Appends the nodes of the current filter to `builder`, reading what a draw paints, and returns the node of its result,
// or the draw itself for a filter that adds no node. A reference to a filter element draws as no filter, as one in a
// document that is not loaded does (html/#dom-context-2d-filter).
PrDrawFilterInput appendFilter(const CanvasState& s, PrDrawFilterBuilder* builder) {
    PrDrawFilterInput input = PrDrawFilterInput_Source;
    for (uint32_t i = 0; i < s.filters.count; ++i) {
        if (s.filters.data[i].kind != PrLayoutFilterKind_Source)
            input = prLayoutFilterAppend(&s.filters.data[i], input, PrImageEdgeMode_None, builder);
    }
    return input;
}

// Returns true if the current filter adds a node to what a draw paints.
bool hasFilter(const CanvasState& s) {
    for (uint32_t i = 0; i < s.filters.count; ++i) {
        if (s.filters.data[i].kind != PrLayoutFilterKind_Source)
            return true;
    }
    return false;
}

// Creates the filter of what a draw paints: the current filter, with one reference for the caller. Returns null for
// none.
const PrDrawFilter* PR_NULLABLE drawFilter(const CanvasState& s) {
    if (!hasFilter(s))
        return nullptr;
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    appendFilter(s, builder);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

// Creates the filter that turns what a draw paints into its shadow (html/#shadows): the alpha of what it paints through
// the current filter, offset, blurred by half the shadow blur, and in the shadow's color.
const PrDrawFilter* shadowFilter(const CanvasState& s) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    float sigma = float(s.shadowBlur / 2);
    PrDrawFilterNode blur{.kind = PrDrawFilterKind_Blur, .inputs = {appendFilter(s, builder)}};
    blur.blur = PrDrawFilterBlur{PrVec2{sigma, sigma}, PrImageEdgeMode_None};
    PrDrawFilterInput blurred = prDrawFilterBuilderAdd(builder, &blur);
    PrDrawFilterNode offset{.kind = PrDrawFilterKind_Offset, .inputs = {blurred}};
    offset.offset = PrVec2{float(s.shadowOffset.x), float(s.shadowOffset.y)};
    PrDrawFilterInput moved = prDrawFilterBuilderAdd(builder, &offset);
    PrDrawFilterNode flood{.kind = PrDrawFilterKind_Flood};
    flood.flood = s.shadowColor;
    PrDrawFilterInput colored = prDrawFilterBuilderAdd(builder, &flood);
    PrDrawFilterNode composite{.kind = PrDrawFilterKind_Composite, .inputs = {colored, moved}};
    composite.composite = PrDrawComposite_SourceIn;
    prDrawFilterBuilderAdd(builder, &composite);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

// Returns true if an operator changes what lies beneath where the source is transparent, so that a draw with it is a
// group over the clipping region (html/#drawing-model). Clear changes only what the draw covers, so that clearing a
// shape erases that shape, which is what content draws it for.
bool unboundedOperation(PrDrawComposite composite) {
    return composite != PrDrawComposite_Clear && prDrawCompositeIsUnbounded(composite);
}

// Draws a shape as the drawing model says (html/#drawing-model): `record` records it under `shapeTransform` with the
// paint that it is passed. `paint` holds the global alpha already. A shadow, and an operator that changes what lies
// beneath a transparent source, are groups in the bitmap's space around the shape.
template <typename Record>
void drawPainted(CanvasContext* c, PrDrawPaint paint, PrDAffine2 shapeTransform, Record record) {
    if (c->pixels == nullptr)
        return;
    const CanvasState& s = c->state;
    const CanvasOperation& operation = kOperations[s.operation];
    syncClips(c);
    auto shape = [&](PrDrawPaint shapePaint) {
        prDrawListPushTransformDAffine2(c->pending, shapeTransform);
        record(shapePaint);
        prDrawListPopTransform(c->pending);
    };
    PrDrawPaint plain = paint;
    plain.composite = PrDrawComposite_SourceOver;
    plain.blendMode = PrDrawBlendMode_Normal;
    if (drawsShadow(s)) {
        const PrDrawFilter* filter = shadowFilter(s);
        PrDrawGroup group = prDrawGroupDefault();
        group.composite = operation.composite;
        group.blendMode = operation.blendMode;
        group.filter = filter;
        prDrawListPushGroup(c->pending, &group);
        shape(plain);
        prDrawListPopGroup(c->pending);
        prDrawFilterRelease(filter);
    }
    const PrDrawFilter* filter = drawFilter(s);
    if (filter != nullptr || unboundedOperation(operation.composite)) {
        PrDrawGroup group = prDrawGroupDefault();
        group.composite = operation.composite;
        group.blendMode = operation.blendMode;
        group.filter = filter;
        prDrawListPushGroup(c->pending, &group);
        shape(plain);
        prDrawListPopGroup(c->pending);
        prDrawFilterRelease(filter);
    } else {
        paint.composite = operation.composite;
        paint.blendMode = operation.blendMode;
        shape(paint);
    }
    c->recorded = true;
    touched(c);
}

// Draws a shape in style `role`, stroked with `stroke` unless it is null, with brush space mapped by `brushTransform`
// into the space of the shape (drawPainted).
template <typename Record>
void drawStyled(CanvasContext* c, PrCanvasStyleRole role, const PrDrawStroke* PR_NULLABLE stroke,
                PrDAffine2 shapeTransform, PrDAffine2 brushTransform, Record record) {
    CanvasPaint painted;
    paintOf(c, role, brushTransform, &painted);
    painted.paint.stroke = stroke;
    drawPainted(c, painted.paint, shapeTransform, record);
}

// Returns a copy of `path` mapped through `transform`.
PrPath* mappedPath(const PrPath* path, PrDAffine2 transform) {
    PrPath* mapped = prPathCreate();
    prPathAddPath(mapped, path, affineOf(transform));
    return mapped;
}

// Strokes `path`, in the space of `transform`, with the line styles (html/#trace-a-path) and the stroke style.
void strokePath(CanvasContext* c, const PrPath* path, PrDAffine2 transform) {
    PrPath* traced = prPathCreate();
    canvasPathPrune(path, traced);
    PrDrawStroke stroke = strokeOf(c);
    drawStyled(c, PrCanvasStyleRole_Stroke, &stroke, transform, prDAffine2Identity(), [&](PrDrawPaint paint) {
        prDrawListPath(c->pending, traced, PrPathFillRule_NonZero, paint);
    });
    prPathDestroy(traced);
}

// Returns the intended path of `path` (html/#intended-path) in the space of the current transform, as a new path that
// the caller frees. The current default path is mapped back from the bitmap's space. Returns null if the transform
// collapses the plane, where the current default path has no such space.
PrPath* PR_NULLABLE userPath(const CanvasContext* c, const CanvasPath* PR_NULLABLE path) {
    if (path != nullptr)
        return prPathClone(path->path);
    PrDAffine2 inverse{};
    if (!prDAffine2Invert(c->state.transform, &inverse))
        return nullptr;
    return mappedPath(c->path.path, inverse);
}

// Adds a stop to a gradient, after the stops at its offset.
void addStop(CanvasGradient* g, PrDrawColorStop stop) {
    uint32_t at = g->stops.count;
    while (at > 0 && g->stops.data[at - 1].offset > stop.offset)
        --at;
    g->stops.insert(at, stop);
}

// Returns the text styles as the text preparation algorithm reads them (html/#text-preparation-algorithm), with what
// inherits taken from `source`.
CanvasTextStyle textStyleOf(const CanvasContext* c, const PrCanvasTextSource& source) {
    const CanvasState& s = c->state;
    double size = double(prCssFontStyle(s.font->css).size);
    PrStringView lang = prStringView(&s.lang);
    PrLocale locale = lang == kInherit ? source.language : lang.size == 0 ? PrLocale(0) : prLocaleFromTag(lang);
    PrTextDirection direction = s.direction == PrCanvasDirection_Ltr   ? PrTextDirection_Ltr
                                : s.direction == PrCanvasDirection_Rtl ? PrTextDirection_Rtl
                                                                       : source.direction;
    // The attributes that override the font's width and capitals do so only where they are not normal.
    PrTextCaps caps = PrTextCaps(s.fontVariantCaps);
    if (caps == PrTextCaps_Normal && prCssFontSmallCaps(s.font->css))
        caps = PrTextCaps_Small;
    float width = s.fontStretch != PrCanvasFontStretch_Normal ? kStretchWidths[s.fontStretch]
                                                              : prCssFontStyle(s.font->css).width;
    return CanvasTextStyle{.font = s.font,
                           .align = s.textAlign,
                           .baseline = s.textBaseline,
                           .direction = direction,
                           .locale = locale,
                           .letterSpacing = spacingPx(s.letterSpacing, size),
                           .wordSpacing = spacingPx(s.wordSpacing, size),
                           .kerning = PrTextKerning(s.fontKerning),
                           .caps = caps,
                           .width = width};
}

// Sets `text` in the text styles into the context's text. Returns false if the context sets no text.
bool setText(CanvasContext* c, PrStringView text) {
    PrCanvasTextSource source{};
    if (c->textSourceFn == nullptr || !c->textSourceFn(c->textSourceContext, &source))
        return false;
    return canvasTextSet(&c->text, text, textStyleOf(c, source), source.fonts, source.shaper);
}

// Fills or strokes text in style `role` (html/#dom-context-2d-filltext): the glyphs placed from (x, y), condensed
// across to `maxWidth` if they are wider.
void drawText(CanvasContext* c, PrStringView source, double x, double y, const double* PR_NULLABLE maxWidth,
              PrCanvasStyleRole role) {
    if (!canvasIsFinite(x, y) || (maxWidth != nullptr && (!canvasIsFinite(*maxWidth) || !(*maxWidth > 0))))
        return;
    if (!setText(c, source) || c->text.runs.count == 0)
        return;
    double scale = maxWidth != nullptr && c->text.width > *maxWidth ? *maxWidth / c->text.width : 1;
    PrDAffine2 local{.a = scale, .d = 1, .e = x, .f = y};
    PrDAffine2 brush{.a = 1 / scale, .d = 1, .e = -x / scale, .f = -y};
    PrDrawStroke stroke = strokeOf(c);
    drawStyled(c, role, role == PrCanvasStyleRole_Stroke ? &stroke : nullptr,
               prDAffine2Concat(local, c->state.transform), brush, [&](PrDrawPaint paint) {
                   for (uint32_t i = 0; i < c->text.runs.count; ++i)
                       prDrawListGlyphs(c->pending, &c->text.runs.data[i], PrVec2{0, 0}, paint);
               });
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrCanvasGradient : CanvasGradient {};
struct PrCanvasPattern : CanvasPattern {};
struct PrCanvasPath2D : CanvasPath {};
struct PrCanvas : CanvasContext {};

PrCanvasSettings prCanvasSettingsDefault(void) {
    return PrCanvasSettings{.alpha = true, .colorSpace = PrColorSpace_Srgb, .colorType = PrCanvasColorType_Unorm8};
}

uint32_t prCanvasPixelFormatSize(PrCanvasPixelFormat format) {
    return format == PrCanvasPixelFormat_RgbaUnorm8 ? 4 : 8;
}

// ---------------------------------------------------------------------------------------------------------------------
// CanvasGradient

PrCanvasGradient* prCanvasGradientCreateLinear(double x0, double y0, double x1, double y1) {
    PR_ASSERT(canvasIsFinite(x0, y0, x1, y1));
    PrCanvasGradient* g = allocZeroed<PrCanvasGradient>();
    g->references = 1;
    g->kind = PrDrawBrushKind_Linear;
    g->start = PrDVec2{x0, y0};
    g->end = PrDVec2{x1, y1};
    return g;
}

PrStatus prCanvasGradientCreateRadial(double x0, double y0, double r0, double x1, double y1, double r1,
                                      PrCanvasGradient* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(canvasIsFinite(x0, y0, r0, x1, y1, r1));
    *out = nullptr;
    if (r0 < 0 || r1 < 0)
        return PrStatus_OutOfRange;
    PrCanvasGradient* g = allocZeroed<PrCanvasGradient>();
    g->references = 1;
    g->kind = PrDrawBrushKind_Radial;
    g->start = PrDVec2{x0, y0};
    g->startRadius = r0;
    g->end = PrDVec2{x1, y1};
    g->endRadius = r1;
    *out = g;
    return PrStatus_Ok;
}

PrCanvasGradient* prCanvasGradientCreateConic(double startAngle, double x, double y) {
    PR_ASSERT(canvasIsFinite(startAngle, x, y));
    PrCanvasGradient* g = allocZeroed<PrCanvasGradient>();
    g->references = 1;
    g->kind = PrDrawBrushKind_Conic;
    g->start = PrDVec2{x, y};
    g->angle = startAngle;
    return g;
}

void prCanvasGradientRetain(PrCanvasGradient* gradient) {
    ++gradient->references;
}

void prCanvasGradientRelease(PrCanvasGradient* PR_NULLABLE gradient) {
    gradientRelease(gradient);
}

PrStatus prCanvasGradientAddColorStop(PrCanvasGradient* gradient, double offset, PrStringView color) {
    PR_ASSERT(!isNanF64(offset));
    if (!(offset >= 0 && offset <= 1))
        return PrStatus_OutOfRange;
    PrColor parsed{};
    if (!parseColor(color, nullptr, &parsed, nullptr))
        return PrStatus_Syntax;
    addStop(gradient, PrDrawColorStop{.offset = float(offset), .color = parsed});
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// CanvasPattern

PrCanvasPattern* prCanvasPatternCreate(const PrCanvasImageSource* source, PrCanvasRepetition repetition) {
    PrCanvasPattern* pattern = allocZeroed<PrCanvasPattern>();
    pattern->references = 1;
    prDrawImageRetain(source->image);
    pattern->image = source->image;
    pattern->extent = source->extent;
    pattern->repetition = repetition;
    pattern->transform = prDAffine2Identity();
    return pattern;
}

void prCanvasPatternRetain(PrCanvasPattern* pattern) {
    ++pattern->references;
}

void prCanvasPatternRelease(PrCanvasPattern* PR_NULLABLE pattern) {
    patternRelease(pattern);
}

void prCanvasPatternSetTransform(PrCanvasPattern* pattern, PrDAffine2 transform) {
    if (affineIsFinite(transform))
        pattern->transform = transform;
}

// ---------------------------------------------------------------------------------------------------------------------
// Path2D

PrCanvasPath2D* prCanvasPath2DCreate(void) {
    PrCanvasPath2D* path = allocZeroed<PrCanvasPath2D>();
    canvasPathInit(path);
    return path;
}

PrCanvasPath2D* prCanvasPath2DCreateFromPathData(PrStringView data) {
    PrCanvasPath2D* path = prCanvasPath2DCreate();
    canvasPathAppendData(path, data);
    return path;
}

PrCanvasPath2D* prCanvasPath2DCreateCopy(const PrCanvasPath2D* path) {
    PrCanvasPath2D* copy = prCanvasPath2DCreate();
    canvasPathCopy(copy, *path);
    return copy;
}

void prCanvasPath2DDestroy(PrCanvasPath2D* PR_NULLABLE path) {
    if (path == nullptr)
        return;
    canvasPathReset(path);
    prMemoryFree(path);
}

void prCanvasPath2DAddPath(PrCanvasPath2D* path, const PrCanvasPath2D* other, PrDAffine2 transform) {
    canvasPathAddPath(path, *other, transform);
}

void prCanvasPath2DClosePath(PrCanvasPath2D* path) {
    canvasPathClose(path);
}

void prCanvasPath2DMoveTo(PrCanvasPath2D* path, double x, double y) {
    canvasPathMoveTo(path, prDAffine2Identity(), x, y);
}

void prCanvasPath2DLineTo(PrCanvasPath2D* path, double x, double y) {
    canvasPathLineTo(path, prDAffine2Identity(), x, y);
}

void prCanvasPath2DQuadraticCurveTo(PrCanvasPath2D* path, double cpx, double cpy, double x, double y) {
    canvasPathQuadraticCurveTo(path, prDAffine2Identity(), cpx, cpy, x, y);
}

void prCanvasPath2DBezierCurveTo(PrCanvasPath2D* path, double cp1x, double cp1y, double cp2x, double cp2y, double x,
                                 double y) {
    canvasPathBezierCurveTo(path, prDAffine2Identity(), cp1x, cp1y, cp2x, cp2y, x, y);
}

PrStatus prCanvasPath2DArcTo(PrCanvasPath2D* path, double x1, double y1, double x2, double y2, double radius) {
    return canvasPathArcTo(path, prDAffine2Identity(), x1, y1, x2, y2, radius);
}

void prCanvasPath2DRect(PrCanvasPath2D* path, double x, double y, double w, double h) {
    canvasPathRect(path, prDAffine2Identity(), x, y, w, h);
}

PrStatus prCanvasPath2DRoundRect(PrCanvasPath2D* path, double x, double y, double w, double h,
                                 const PrDVec2* PR_NULLABLE radii, uint32_t radiusCount) {
    return canvasPathRoundRect(path, prDAffine2Identity(), x, y, w, h, radii, radiusCount);
}

PrStatus prCanvasPath2DArc(PrCanvasPath2D* path, double x, double y, double radius, double startAngle,
                           double endAngle, bool counterclockwise) {
    return canvasPathEllipse(path, prDAffine2Identity(), x, y, radius, radius, 0, startAngle, endAngle,
                             counterclockwise);
}

PrStatus prCanvasPath2DEllipse(PrCanvasPath2D* path, double x, double y, double radiusX, double radiusY,
                               double rotation, double startAngle, double endAngle, bool counterclockwise) {
    return canvasPathEllipse(path, prDAffine2Identity(), x, y, radiusX, radiusY, rotation, startAngle, endAngle,
                             counterclockwise);
}

// ---------------------------------------------------------------------------------------------------------------------
// CanvasRenderingContext2D

PrCanvas* prCanvasCreate(const PrCanvasDesc* desc) {
    PR_ASSERT(desc->settings.colorSpace == PrColorSpace_Srgb || desc->settings.colorSpace == PrColorSpace_DisplayP3);
    PrCanvas* c = allocZeroed<PrCanvas>();
    c->settings = desc->settings;
    c->element = desc->element;
    // A bitmap holds 8 bits a channel only for unorm8 sRGB. Half floats hold the range of any other: float16 color, and
    // a Display P3 color space, whose colors are extended sRGB outside sRGB's gamut.
    bool bytes = c->settings.colorType == PrCanvasColorType_Unorm8 && c->settings.colorSpace == PrColorSpace_Srgb;
    c->format = bytes ? PrPixelFormat_Rgba8 : PrPixelFormat_RgbaF16;
    c->originClean = true;
    c->pending = prDrawListCreate();
    c->device = desc->device;
    c->textSourceFn = desc->textSource;
    c->textSourceContext = desc->textSourceContext;
    PrCssFont* font = nullptr;
    prCssFontCreate(kDefaultFont, nullptr, &font);
    PR_ASSERT(font != nullptr);
    c->defaultFont = canvasFontCreate(font);
    canvasTextInit(&c->text);
    stateInit(&c->state, c->defaultFont);
    canvasPathInit(&c->path);
    allocateBitmap(c, desc->extent);
    c->changedFn = desc->changed;
    c->changedContext = desc->changedContext;
    return c;
}

void prCanvasDestroy(PrCanvas* PR_NULLABLE canvas) {
    if (canvas == nullptr)
        return;
    discard(canvas);
    prDrawListRelease(canvas->pending);
    segmentsReset(&canvas->segments);
    prDrawImageRelease(canvas->snapshot);
    // The renderer's resources on the device last until the device finishes what the renderer drew.
    if (canvas->device != nullptr && canvas->frame != nullptr)
        prGpuDeviceWaitIdle(canvas->device);
    prDrawFrameDestroy(canvas->frame);
    prDrawListRelease(canvas->drawnList);
    prDrawRendererDestroy(canvas->renderer);
    // In memory, the pixels are the bitmap's.
    if (canvas->device != nullptr)
        prMemoryFree(canvas->pixels);
    bitmapRelease(canvas->bitmap);
    stateReset(&canvas->state);
    for (uint32_t i = 0; i < canvas->saved.count; ++i)
        stateReset(&canvas->saved.data[i]);
    canvas->saved.reset();
    canvas->openClips.reset();
    canvas->dashes.reset();
    canvasPathReset(&canvas->path);
    canvasTextReset(&canvas->text);
    canvasFontRelease(canvas->defaultFont);
    prMemoryFree(canvas);
}

PrUVec2 prCanvasExtent(const PrCanvas* canvas) {
    return canvas->extent;
}

void prCanvasSetExtent(PrCanvas* canvas, PrUVec2 extent) {
    prCanvasReset(canvas);
    if (extent.x != canvas->extent.x || extent.y != canvas->extent.y)
        allocateBitmap(canvas, extent);
}

uint64_t prCanvasVersion(const PrCanvas* canvas) {
    return canvas->version;
}

const PrDrawImage* PR_NULLABLE prCanvasSnapshot(PrCanvas* canvas) {
    readPixels(canvas);
    canvas->notified = false;
    if (canvas->pixels == nullptr)
        return nullptr;
    if (canvas->snapshot == nullptr) {
        PrDrawImageDesc desc{.pixels = prPixelsView(pixelsOf(canvas)), .format = canvas->format,
                             .alpha = PrDrawAlpha(canvas->settings.alpha ? PrDrawAlpha_Premultiplied
                                                                         : PrDrawAlpha_Opaque),
                             .space = PrColorSpace_Srgb};
        const PrDrawImage* image = nullptr;
        if (prDrawImageCreate(&desc, &image) != PrStatus_Ok)
            return nullptr;
        canvas->snapshot = image;
    }
    prDrawImageRetain(canvas->snapshot);
    return canvas->snapshot;
}

PrCanvasPresentation prCanvasPresent(PrCanvas* canvas, PrDrawSampling sampling) {
    canvas->notified = false;
    PrCanvasPresentation presentation{};
    if (canvas->bitmap == nullptr)
        return presentation;
    CanvasShown* shown = show(canvas, sampling);
    presentation.painter = PrLayoutPainter{.paint = paintShown, .context = shown, .retain = shownRetain,
                                           .release = shownRelease};
    presentation.revision = canvas->revision;
    return presentation;
}

bool prCanvasIsOriginClean(const PrCanvas* canvas) {
    return canvas->originClean;
}

PrCanvasSettings prCanvasGetContextAttributes(const PrCanvas* canvas) {
    return canvas->settings;
}

void prCanvasSave(PrCanvas* canvas) {
    CanvasState copy;
    stateCopy(&copy, canvas->state);
    canvas->saved.push(copy);
}

void prCanvasRestore(PrCanvas* canvas) {
    if (canvas->saved.count == 0)
        return;
    stateReset(&canvas->state);
    canvas->state = canvas->saved.last();
    canvas->saved.pop();
}

void prCanvasReset(PrCanvas* canvas) {
    discard(canvas);
    clearBitmap(canvas);
    canvasPathClear(&canvas->path);
    for (uint32_t i = 0; i < canvas->saved.count; ++i)
        stateReset(&canvas->saved.data[i]);
    canvas->saved.clear();
    stateReset(&canvas->state);
    stateInit(&canvas->state, canvas->defaultFont);
}

bool prCanvasIsContextLost(const PrCanvas* canvas) {
    (void)canvas;
    return false;
}

void prCanvasScale(PrCanvas* canvas, double x, double y) {
    prCanvasTransform(canvas, PrDAffine2{.a = x, .d = y});
}

void prCanvasRotate(PrCanvas* canvas, double angle) {
    if (!canvasIsFinite(angle))
        return;
    double cosine = cosF64(angle);
    double sine = sinF64(angle);
    prCanvasTransform(canvas, PrDAffine2{.a = cosine, .b = sine, .c = -sine, .d = cosine});
}

void prCanvasTranslate(PrCanvas* canvas, double x, double y) {
    prCanvasTransform(canvas, PrDAffine2{.a = 1, .d = 1, .e = x, .f = y});
}

void prCanvasTransform(PrCanvas* canvas, PrDAffine2 transform) {
    if (affineIsFinite(transform))
        canvas->state.transform = prDAffine2Concat(transform, canvas->state.transform);
}

void prCanvasSetTransform(PrCanvas* canvas, PrDAffine2 transform) {
    if (affineIsFinite(transform))
        canvas->state.transform = transform;
}

void prCanvasResetTransform(PrCanvas* canvas) {
    canvas->state.transform = prDAffine2Identity();
}

PrDAffine2 prCanvasGetTransform(const PrCanvas* canvas) {
    return canvas->state.transform;
}

double prCanvasGlobalAlpha(const PrCanvas* canvas) {
    return canvas->state.globalAlpha;
}

void prCanvasSetGlobalAlpha(PrCanvas* canvas, double value) {
    if (value >= 0 && value <= 1)
        canvas->state.globalAlpha = value;
}

PrStringView prCanvasGlobalCompositeOperation(const PrCanvas* canvas) {
    return kOperations[canvas->state.operation].name;
}

void prCanvasSetGlobalCompositeOperation(PrCanvas* canvas, PrStringView value) {
    for (uint32_t i = 0; i < countOf(kOperations); ++i) {
        if (value == kOperations[i].name) {
            canvas->state.operation = i;
            return;
        }
    }
}

bool prCanvasImageSmoothingEnabled(const PrCanvas* canvas) {
    return canvas->state.smoothing;
}

void prCanvasSetImageSmoothingEnabled(PrCanvas* canvas, bool value) {
    canvas->state.smoothing = value;
}

PrCanvasImageSmoothingQuality prCanvasImageSmoothingQuality(
    const PrCanvas* canvas) {
    return canvas->state.smoothingQuality;
}

void prCanvasSetImageSmoothingQuality(PrCanvas* canvas,
                                      PrCanvasImageSmoothingQuality value) {
    canvas->state.smoothingQuality = value;
}

PrCanvasStyle prCanvasStyle(const PrCanvas* canvas, PrCanvasStyleRole role) {
    const CanvasStyleValue& style = canvas->state.styles[role];
    PrCanvasStyle out{.kind = style.kind};
    if (style.kind == PrCanvasStyleKind_Color)
        out.color = prStringView(&style.text);
    else if (style.kind == PrCanvasStyleKind_Gradient)
        out.gradient = static_cast<PrCanvasGradient*>(style.gradient);
    else
        out.pattern = static_cast<PrCanvasPattern*>(style.pattern);
    return out;
}

void prCanvasSetStyleColor(PrCanvas* canvas, PrCanvasStyleRole role,
                           PrStringView color) {
    PrColor parsed{};
    PrString text = prStringNull();
    if (!parseColor(color, canvas->element, &parsed, &text))
        return;
    CanvasStyleValue* style = &canvas->state.styles[role];
    styleReset(style);
    *style = CanvasStyleValue{.kind = PrCanvasStyleKind_Color, .color = parsed, .text = text};
}

void prCanvasSetStyleGradient(PrCanvas* canvas, PrCanvasStyleRole role,
                              PrCanvasGradient* gradient) {
    ++gradient->references;
    CanvasStyleValue* style = &canvas->state.styles[role];
    styleReset(style);
    *style = CanvasStyleValue{.kind = PrCanvasStyleKind_Gradient, .gradient = gradient};
}

void prCanvasSetStylePattern(PrCanvas* canvas, PrCanvasStyleRole role,
                             PrCanvasPattern* pattern) {
    ++pattern->references;
    CanvasStyleValue* style = &canvas->state.styles[role];
    styleReset(style);
    *style = CanvasStyleValue{.kind = PrCanvasStyleKind_Pattern, .pattern = pattern};
}

PrDVec2 prCanvasShadowOffset(const PrCanvas* canvas) {
    return canvas->state.shadowOffset;
}

void prCanvasSetShadowOffset(PrCanvas* canvas, PrDVec2 value) {
    if (canvasIsFinite(value.x))
        canvas->state.shadowOffset.x = value.x;
    if (canvasIsFinite(value.y))
        canvas->state.shadowOffset.y = value.y;
}

double prCanvasShadowBlur(const PrCanvas* canvas) {
    return canvas->state.shadowBlur;
}

void prCanvasSetShadowBlur(PrCanvas* canvas, double value) {
    if (canvasIsFinite(value) && value >= 0)
        canvas->state.shadowBlur = value;
}

PrStringView prCanvasShadowColor(const PrCanvas* canvas) {
    return prStringView(&canvas->state.shadowText);
}

void prCanvasSetShadowColor(PrCanvas* canvas, PrStringView value) {
    PrColor parsed{};
    PrString text = prStringNull();
    if (!parseColor(value, canvas->element, &parsed, &text))
        return;
    prStringReset(&canvas->state.shadowText);
    canvas->state.shadowText = text;
    canvas->state.shadowColor = parsed;
}

PrStringView prCanvasFilter(const PrCanvas* canvas) {
    return prStringView(&canvas->state.filterText);
}

void prCanvasSetFilter(PrCanvas* canvas, PrStringView value) {
    Array<PrLayoutFilter>& filters = canvas->state.filters;
    if (value == kNone) {
        filters.clear();
        prStringReplace(&canvas->state.filterText, value);
        return;
    }
    uint32_t count = 0;
    if (prCssParseFilterList(value, canvas->element, nullptr, 0, &count) != PrStatus_Ok)
        return;
    filters.resize(count);
    prCssParseFilterList(value, canvas->element, filters.data, count, &count);
    prStringReplace(&canvas->state.filterText, value);
}

void prCanvasClearRect(PrCanvas* canvas, double x, double y, double w, double h) {
    if (!canvasIsFinite(x, y, w, h) || w == 0 || h == 0 || canvas->pixels == nullptr)
        return;
    syncClips(canvas);
    PrRect rect{float(minF64(x, x + w)), float(minF64(y, y + h)), float(maxF64(x, x + w)), float(maxF64(y, y + h))};
    prDrawListPushTransformDAffine2(canvas->pending, canvas->state.transform);
    prDrawListClearRect(canvas->pending, rect, PrColor{0, 0, 0, 0});
    prDrawListPopTransform(canvas->pending);
    canvas->recorded = true;
    touched(canvas);
}

void prCanvasFillRect(PrCanvas* canvas, double x, double y, double w, double h) {
    if (!canvasIsFinite(x, y, w, h) || w == 0 || h == 0)
        return;
    PrRect rect{float(minF64(x, x + w)), float(minF64(y, y + h)), float(maxF64(x, x + w)), float(maxF64(y, y + h))};
    drawStyled(canvas, PrCanvasStyleRole_Fill, nullptr, canvas->state.transform, prDAffine2Identity(),
               [&](PrDrawPaint paint) { prDrawListRect(canvas->pending, rect, paint); });
}

void prCanvasStrokeRect(PrCanvas* canvas, double x, double y, double w,
                        double h) {
    if (!canvasIsFinite(x, y, w, h) || (w == 0 && h == 0))
        return;
    // A rectangle with one side of zero length is a line, which a stroke caps at both ends instead of joining.
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{float(x), float(y)});
    if (w == 0 || h == 0) {
        prPathLineTo(path, PrVec2{float(x + w), float(y + h)});
    } else {
        prPathLineTo(path, PrVec2{float(x + w), float(y)});
        prPathLineTo(path, PrVec2{float(x + w), float(y + h)});
        prPathLineTo(path, PrVec2{float(x), float(y + h)});
        prPathClose(path);
    }
    strokePath(canvas, path, canvas->state.transform);
    prPathDestroy(path);
}

void prCanvasBeginPath(PrCanvas* canvas) {
    canvasPathClear(&canvas->path);
}

void prCanvasFill(PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path,
                  PrPathFillRule rule) {
    // The current default path is in the bitmap's space already, and the brush is in the current transform's space.
    const PrPath* shape = path != nullptr ? path->path : canvas->path.path;
    PrDAffine2 shapeTransform = path != nullptr ? canvas->state.transform : prDAffine2Identity();
    PrDAffine2 brushTransform = path != nullptr ? prDAffine2Identity() : canvas->state.transform;
    drawStyled(canvas, PrCanvasStyleRole_Fill, nullptr, shapeTransform, brushTransform,
               [&](PrDrawPaint paint) { prDrawListPath(canvas->pending, shape, rule, paint); });
}

void prCanvasStroke(PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path) {
    PrPath* shape = userPath(canvas, path);
    if (shape == nullptr)
        return;
    strokePath(canvas, shape, canvas->state.transform);
    prPathDestroy(shape);
}

void prCanvasClip(PrCanvas* canvas, const PrCanvasPath2D* PR_NULLABLE path,
                  PrPathFillRule rule) {
    PrPath* area = path != nullptr ? mappedPath(path->path, canvas->state.transform) : prPathClone(canvas->path.path);
    canvas->state.clips.push(CanvasClip{area, ++canvas->nextClipId, rule});
}

bool prCanvasIsPointInPath(const PrCanvas* canvas,
                           const PrCanvasPath2D* PR_NULLABLE path, double x, double y,
                           PrPathFillRule rule) {
    if (!canvasIsFinite(x, y))
        return false;
    PrVec2 point{float(x), float(y)};
    if (path == nullptr)
        return canvasPathContains(canvas->path.path, point, rule);
    PrPath* mapped = mappedPath(path->path, canvas->state.transform);
    bool inside = canvasPathContains(mapped, point, rule);
    prPathDestroy(mapped);
    return inside;
}

bool prCanvasIsPointInStroke(const PrCanvas* canvas,
                             const PrCanvasPath2D* PR_NULLABLE path, double x, double y) {
    PrDAffine2 inverse{};
    if (!canvasIsFinite(x, y) || !prDAffine2Invert(canvas->state.transform, &inverse))
        return false;
    PrPath* shape = userPath(canvas, path);
    if (shape == nullptr)
        return false;
    PrDVec2 point = canvasMapPoint(inverse, x, y);
    PrDrawStroke stroke = strokeOf(const_cast<PrCanvas*>(canvas));
    bool inside = canvasPathStrokeContains(shape, &stroke.shape, PrVec2{float(point.x), float(point.y)});
    prPathDestroy(shape);
    return inside;
}

void prCanvasDrawFocusRing(PrCanvas* canvas,
                           const PrCanvasPath2D* PR_NULLABLE path) {
    PrPath* shape = userPath(canvas, path);
    if (shape == nullptr)
        return;
    // The ring is a dark outline two pixels wide, with no shadow, global alpha or operator.
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 2;
    stroke.nonScaling = true;
    PrDrawPaint paint{.color = PrColor{0.0625f, 0.0625f, 0.0625f, 1}, .stroke = &stroke};
    if (canvas->pixels != nullptr) {
        syncClips(canvas);
        prDrawListPushTransformDAffine2(canvas->pending, canvas->state.transform);
        prDrawListPath(canvas->pending, shape, PrPathFillRule_NonZero, paint);
        prDrawListPopTransform(canvas->pending);
        canvas->recorded = true;
        touched(canvas);
    }
    prPathDestroy(shape);
}

void prCanvasFillText(PrCanvas* canvas, PrStringView text, double x, double y,
                      const double* PR_NULLABLE maxWidth) {
    drawText(canvas, text, x, y, maxWidth, PrCanvasStyleRole_Fill);
}

void prCanvasStrokeText(PrCanvas* canvas, PrStringView text, double x, double y,
                        const double* PR_NULLABLE maxWidth) {
    drawText(canvas, text, x, y, maxWidth, PrCanvasStyleRole_Stroke);
}

PrCanvasTextMetrics prCanvasMeasureText(PrCanvas* canvas, PrStringView text) {
    if (!setText(canvas, text))
        return PrCanvasTextMetrics{};
    const CanvasText& t = canvas->text;
    bool inked = !prRectIsEmpty(t.ink);
    return PrCanvasTextMetrics{.width = t.width,
                               .actualBoundingBoxLeft = inked ? -double(t.ink.left) : 0,
                               .actualBoundingBoxRight = inked ? double(t.ink.right) : 0,
                               .fontBoundingBoxAscent = -t.ascent,
                               .fontBoundingBoxDescent = t.descent,
                               .actualBoundingBoxAscent = inked ? -double(t.ink.top) : 0,
                               .actualBoundingBoxDescent = inked ? double(t.ink.bottom) : 0,
                               .emHeightAscent = -t.emTop,
                               .emHeightDescent = t.emBottom,
                               .hangingBaseline = -t.hanging,
                               .alphabeticBaseline = -t.alphabetic,
                               .ideographicBaseline = -t.ideographic};
}

void prCanvasDrawImage(PrCanvas* canvas, const PrCanvasImageSource* source,
                       double sx, double sy, double sw, double sh, double dx, double dy, double dw,
                       double dh) {
    if (!canvasIsFinite(sx, sy, sw, sh, dx, dy, dw, dh) || sw == 0 || sh == 0 || dw == 0 || dh == 0)
        return;
    if (!source->originClean)
        canvas->originClean = false;
    // A negative extent counts from the far edge; the image is not flipped.
    if (sw < 0) {
        sx += sw;
        sw = -sw;
    }
    if (sh < 0) {
        sy += sh;
        sh = -sh;
    }
    if (dw < 0) {
        dx += dw;
        dw = -dw;
    }
    if (dh < 0) {
        dy += dh;
        dh = -dh;
    }
    // The source rectangle is clipped to the image, and the destination with it in proportion.
    double scaleX = dw / sw;
    double scaleY = dh / sh;
    double left = maxF64(sx, 0);
    double top = maxF64(sy, 0);
    double right = minF64(sx + sw, double(source->extent.x));
    double bottom = minF64(sy + sh, double(source->extent.y));
    if (right <= left || bottom <= top)
        return;
    double destLeft = dx + (left - sx) * scaleX;
    double destTop = dy + (top - sy) * scaleY;
    double destRight = dx + (right - sx) * scaleX;
    double destBottom = dy + (bottom - sy) * scaleY;
    // The source's CSS pixels map onto its image's pixels.
    PrUVec2 pixels = prDrawImageExtent(source->image);
    float texelsX = float(pixels.x) / source->extent.x;
    float texelsY = float(pixels.y) / source->extent.y;
    PrRect texels{float(left) * texelsX, float(top) * texelsY, float(right) * texelsX, float(bottom) * texelsY};
    PrRect destination{float(destLeft), float(destTop), float(destRight), float(destBottom)};
    PrDrawSampling sampling = PrDrawSampling(samplingOf(canvas->state) | PrDrawSamplingFlag_Gutter);
    PrDrawBrush brush = prDrawBrushImage(prDrawSourceImage(source->image, texels), destination, sampling);
    brush.opacity = float(canvas->state.globalAlpha);
    PrDrawPaint paint{.color = PrColor{1, 1, 1, float(canvas->state.globalAlpha)}, .brush = &brush};
    drawPainted(canvas, paint, canvas->state.transform,
                [&](PrDrawPaint painted) { prDrawListRect(canvas->pending, destination, painted); });
}

void prCanvasGetImageData(PrCanvas* canvas, PrIVec2 origin,
                          const PrCanvasImageData* data, void* out) {
    readPixels(canvas);
    uint32_t pixelSize = prCanvasPixelFormatSize(data->format);
    memZero(out, size_t(data->extent.x) * data->extent.y * pixelSize);
    if (canvas->pixels == nullptr)
        return;
    bool exact = holdsBytes(canvas) && data->format == PrCanvasPixelFormat_RgbaUnorm8
                 && data->colorSpace == PrColorSpace_Srgb;
    for (uint32_t y = 0; y < data->extent.y; ++y) {
        int64_t row = int64_t(origin.y) + y;
        if (row < 0 || row >= int64_t(canvas->extent.y))
            continue;
        for (uint32_t x = 0; x < data->extent.x; ++x) {
            int64_t column = int64_t(origin.x) + x;
            if (column < 0 || column >= int64_t(canvas->extent.x))
                continue;
            size_t from = size_t(row) * canvas->extent.x + size_t(column);
            size_t to = size_t(y) * data->extent.x + x;
            if (exact) {
                // Bytes are unpremultiplied by rounding each channel to the nearest whole number.
                const uint8_t* q = canvas->pixels + from * 4;
                uint8_t* p = static_cast<uint8_t*>(out) + to * 4;
                uint32_t a = q[3];
                for (uint32_t i = 0; i < 3; ++i) {
                    uint32_t straight = a == 0 ? 0 : (q[i] * 255u + a / 2) / a;
                    p[i] = uint8_t(straight > 255 ? 255 : straight);
                }
                p[3] = uint8_t(a);
                continue;
            }
            PrColor color = convertColor(readPixel(canvas, from), PrColorSpace_Srgb, data->colorSpace);
            writeData(out, data->format, to, color);
        }
    }
}

void prCanvasPutImageData(PrCanvas* canvas, const PrCanvasImageData* data,
                          const void* pixels, PrIVec2 origin, int32_t dirtyX, int32_t dirtyY,
                          int32_t dirtyWidth, int32_t dirtyHeight) {
    readPixels(canvas);
    int64_t x0 = dirtyX;
    int64_t y0 = dirtyY;
    int64_t x1 = int64_t(dirtyX) + dirtyWidth;
    int64_t y1 = int64_t(dirtyY) + dirtyHeight;
    if (x1 < x0) {
        int64_t swap = x0;
        x0 = x1;
        x1 = swap;
    }
    if (y1 < y0) {
        int64_t swap = y0;
        y0 = y1;
        y1 = swap;
    }
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > int64_t(data->extent.x) ? int64_t(data->extent.x) : x1;
    y1 = y1 > int64_t(data->extent.y) ? int64_t(data->extent.y) : y1;
    if (canvas->pixels == nullptr || x1 <= x0 || y1 <= y0)
        return;
    bool exact = holdsBytes(canvas) && data->format == PrCanvasPixelFormat_RgbaUnorm8
                 && data->colorSpace == PrColorSpace_Srgb;
    for (int64_t y = y0; y < y1; ++y) {
        int64_t row = origin.y + y;
        if (row < 0 || row >= int64_t(canvas->extent.y))
            continue;
        for (int64_t x = x0; x < x1; ++x) {
            int64_t column = origin.x + x;
            if (column < 0 || column >= int64_t(canvas->extent.x))
                continue;
            size_t from = size_t(y) * data->extent.x + size_t(x);
            size_t to = size_t(row) * canvas->extent.x + size_t(column);
            if (exact) {
                // Bytes are premultiplied by rounding each channel to the nearest whole number.
                const uint8_t* p = static_cast<const uint8_t*>(pixels) + from * 4;
                uint8_t* q = canvas->pixels + to * 4;
                uint32_t a = canvas->settings.alpha ? p[3] : 255;
                for (uint32_t i = 0; i < 3; ++i)
                    q[i] = uint8_t((p[i] * a + 127) / 255);
                q[3] = uint8_t(a);
                continue;
            }
            PrColor color = convertColor(readData(pixels, data->format, from), data->colorSpace, PrColorSpace_Srgb);
            writePixel(canvas, to, color);
        }
    }
    writePixels(canvas);
    changed(canvas);
}

double prCanvasLineWidth(const PrCanvas* canvas) {
    return canvas->state.lineWidth;
}

void prCanvasSetLineWidth(PrCanvas* canvas, double value) {
    if (canvasIsFinite(value) && value > 0)
        canvas->state.lineWidth = value;
}

double prCanvasMiterLimit(const PrCanvas* canvas) {
    return canvas->state.miterLimit;
}

void prCanvasSetMiterLimit(PrCanvas* canvas, double value) {
    if (canvasIsFinite(value) && value > 0)
        canvas->state.miterLimit = value;
}

PrCanvasLineCap prCanvasLineCap(const PrCanvas* canvas) {
    return canvas->state.lineCap;
}

void prCanvasSetLineCap(PrCanvas* canvas, PrCanvasLineCap value) {
    canvas->state.lineCap = value;
}

PrCanvasLineJoin prCanvasLineJoin(const PrCanvas* canvas) {
    return canvas->state.lineJoin;
}

void prCanvasSetLineJoin(PrCanvas* canvas, PrCanvasLineJoin value) {
    canvas->state.lineJoin = value;
}

void prCanvasSetLineDash(PrCanvas* canvas, const double* PR_NULLABLE segments,
                         uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!canvasIsFinite(segments[i]) || segments[i] < 0)
            return;
    }
    Array<double>& dashes = canvas->state.dashes;
    dashes.clear();
    if (count == 0)
        return;
    dashes.append(segments, count);
    if (count % 2 == 1)
        dashes.append(segments, count);
}

uint32_t prCanvasLineDashCount(const PrCanvas* canvas) {
    return canvas->state.dashes.count;
}

const double* PR_NULLABLE prCanvasGetLineDash(const PrCanvas* canvas) {
    return canvas->state.dashes.data;
}

double prCanvasLineDashOffset(const PrCanvas* canvas) {
    return canvas->state.dashOffset;
}

void prCanvasSetLineDashOffset(PrCanvas* canvas, double value) {
    if (canvasIsFinite(value))
        canvas->state.dashOffset = value;
}

PrStringView prCanvasLang(const PrCanvas* canvas) {
    return prStringView(&canvas->state.lang);
}

void prCanvasSetLang(PrCanvas* canvas, PrStringView value) {
    prStringReplace(&canvas->state.lang, value);
}

PrStringView prCanvasFont(const PrCanvas* canvas) {
    return prCssFontSerialization(canvas->state.font->css);
}

void prCanvasSetFont(PrCanvas* canvas, PrStringView value) {
    PrCssFont* font = nullptr;
    if (prCssFontCreate(value, canvas->element, &font) != PrStatus_Ok)
        return;
    canvasFontRelease(canvas->state.font);
    canvas->state.font = canvasFontCreate(font);
}

PrCanvasTextAlign prCanvasTextAlign(const PrCanvas* canvas) {
    return canvas->state.textAlign;
}

void prCanvasSetTextAlign(PrCanvas* canvas, PrCanvasTextAlign value) {
    canvas->state.textAlign = value;
}

PrCanvasTextBaseline prCanvasTextBaseline(const PrCanvas* canvas) {
    return canvas->state.textBaseline;
}

void prCanvasSetTextBaseline(PrCanvas* canvas, PrCanvasTextBaseline value) {
    canvas->state.textBaseline = value;
}

PrCanvasDirection prCanvasDirection(const PrCanvas* canvas) {
    return canvas->state.direction;
}

void prCanvasSetDirection(PrCanvas* canvas, PrCanvasDirection value) {
    canvas->state.direction = value;
}

PrStringView prCanvasLetterSpacing(const PrCanvas* canvas) {
    return prStringView(&canvas->state.letterSpacing.text);
}

void prCanvasSetLetterSpacing(PrCanvas* canvas, PrStringView value) {
    CanvasSpacing spacing{};
    if (!parseSpacing(value, &spacing))
        return;
    prStringReset(&canvas->state.letterSpacing.text);
    canvas->state.letterSpacing = spacing;
}

PrStringView prCanvasWordSpacing(const PrCanvas* canvas) {
    return prStringView(&canvas->state.wordSpacing.text);
}

void prCanvasSetWordSpacing(PrCanvas* canvas, PrStringView value) {
    CanvasSpacing spacing{};
    if (!parseSpacing(value, &spacing))
        return;
    prStringReset(&canvas->state.wordSpacing.text);
    canvas->state.wordSpacing = spacing;
}

PrCanvasFontKerning prCanvasFontKerning(const PrCanvas* canvas) {
    return canvas->state.fontKerning;
}

void prCanvasSetFontKerning(PrCanvas* canvas, PrCanvasFontKerning value) {
    canvas->state.fontKerning = value;
}

PrCanvasFontStretch prCanvasFontStretch(const PrCanvas* canvas) {
    return canvas->state.fontStretch;
}

void prCanvasSetFontStretch(PrCanvas* canvas, PrCanvasFontStretch value) {
    canvas->state.fontStretch = value;
}

PrCanvasFontVariantCaps prCanvasFontVariantCaps(const PrCanvas* canvas) {
    return canvas->state.fontVariantCaps;
}

void prCanvasSetFontVariantCaps(PrCanvas* canvas, PrCanvasFontVariantCaps value) {
    canvas->state.fontVariantCaps = value;
}

PrCanvasTextRendering prCanvasTextRendering(const PrCanvas* canvas) {
    return canvas->state.textRendering;
}

void prCanvasSetTextRendering(PrCanvas* canvas, PrCanvasTextRendering value) {
    canvas->state.textRendering = value;
}

void prCanvasClosePath(PrCanvas* canvas) {
    canvasPathClose(&canvas->path);
}

void prCanvasMoveTo(PrCanvas* canvas, double x, double y) {
    canvasPathMoveTo(&canvas->path, canvas->state.transform, x, y);
}

void prCanvasLineTo(PrCanvas* canvas, double x, double y) {
    canvasPathLineTo(&canvas->path, canvas->state.transform, x, y);
}

void prCanvasQuadraticCurveTo(PrCanvas* canvas, double cpx, double cpy, double x,
                              double y) {
    canvasPathQuadraticCurveTo(&canvas->path, canvas->state.transform, cpx, cpy, x, y);
}

void prCanvasBezierCurveTo(PrCanvas* canvas, double cp1x, double cp1y,
                           double cp2x, double cp2y, double x, double y) {
    canvasPathBezierCurveTo(&canvas->path, canvas->state.transform, cp1x, cp1y, cp2x, cp2y, x, y);
}

PrStatus prCanvasArcTo(PrCanvas* canvas, double x1, double y1, double x2,
                       double y2, double radius) {
    return canvasPathArcTo(&canvas->path, canvas->state.transform, x1, y1, x2, y2, radius);
}

void prCanvasRect(PrCanvas* canvas, double x, double y, double w, double h) {
    canvasPathRect(&canvas->path, canvas->state.transform, x, y, w, h);
}

PrStatus prCanvasRoundRect(PrCanvas* canvas, double x, double y, double w,
                           double h, const PrDVec2* PR_NULLABLE radii, uint32_t radiusCount) {
    return canvasPathRoundRect(&canvas->path, canvas->state.transform, x, y, w, h, radii, radiusCount);
}

PrStatus prCanvasArc(PrCanvas* canvas, double x, double y, double radius,
                     double startAngle, double endAngle, bool counterclockwise) {
    return canvasPathEllipse(&canvas->path, canvas->state.transform, x, y, radius, radius, 0, startAngle, endAngle,
                             counterclockwise);
}

PrStatus prCanvasEllipse(PrCanvas* canvas, double x, double y, double radiusX,
                         double radiusY, double rotation, double startAngle, double endAngle,
                         bool counterclockwise) {
    return canvasPathEllipse(&canvas->path, canvas->state.transform, x, y, radiusX, radiusY, rotation, startAngle,
                             endAngle, counterclockwise);
}
