// The SVG draw writer, which writes each page's draw list as an SVG 2 document of its own, since SVG has no pages.
//
// SVG2 - https://www.w3.org/TR/SVG2/

#include "pr/draw_svg.h"

#include "pr/alloc.h"
#include "pr/base64.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_writer_backend.h"
#include "pr/float.h"
#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"

#include "draw/backends/shared/src/draw_shared_writer.h"

namespace pr_impl {

namespace {

// A glyph whose outline the document defines, by its font's id and its id in the font.
struct SvgGlyph {
    uint64_t font;
    uint16_t id;
    bool outlined;              // true if the glyph has an outline, which a path defines
};

struct SvgWriter : PrDrawWriter {
    PrDrawWriterOptions options;
    DrawSharedWalker walker;
    PrDrawDocumentDesc document;
    PrTextBuffer defs{};
    PrTextBuffer body{};
    PrTextBuffer scratch{};
    RawArray<uint8_t> rgba;
    PrTextBuffer encoded{};
    RawArray<DrawSharedStop> stops;
    RawArray<SvgGlyph> glyphs;
    HashIndex glyphIndex;
    PrPath* path;
    uint32_t nextId;
    uint32_t page;              // the index of the next page, which is the output that it is written to
};

uint32_t glyphHash(uint64_t font, uint16_t id) {
    return KeyIndex::hashOf(font * 65537 + id);
}

// Appends `text` escaped for XML's character data and attribute values.
void escaped(PrTextBuffer& out, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        switch (c) {
            case '&': prTextBufferAppend(&out, "&amp;"); break;
            case '<': prTextBufferAppend(&out, "&lt;"); break;
            case '>': prTextBufferAppend(&out, "&gt;"); break;
            case '"': prTextBufferAppend(&out, "&quot;"); break;
            default: prTextBufferAppendChar(&out, c); break;
        }
    }
}

uint32_t nextId(SvgWriter& s) {
    return ++s.nextId;
}

// Appends ` name="prefix<id>"`.
void idAttribute(PrTextBuffer& out, PrStringView name, char prefix, uint32_t id) {
    prTextBufferAppendChar(&out, ' ');
    prTextBufferAppend(&out, name);
    prTextBufferAppend(&out, "=\"");
    prTextBufferAppendChar(&out, prefix);
    prTextBufferAppendU64(&out, id, 10);
    prTextBufferAppendChar(&out, '"');
}

// Appends ` name="url(#<prefix><id>)"`.
void urlAttribute(PrTextBuffer& out, PrStringView name, char prefix, uint32_t id) {
    prTextBufferAppendChar(&out, ' ');
    prTextBufferAppend(&out, name);
    prTextBufferAppend(&out, "=\"url(#");
    prTextBufferAppendChar(&out, prefix);
    prTextBufferAppendU64(&out, id, 10);
    prTextBufferAppend(&out, ")\"");
}

void numberAttribute(PrTextBuffer& out, PrStringView name, double value) {
    prTextBufferAppendChar(&out, ' ');
    prTextBufferAppend(&out, name);
    prTextBufferAppend(&out, "=\"");
    drawSharedAppendNumber(out, value);
    prTextBufferAppendChar(&out, '"');
}

// Appends a transform attribute, unless the transform is the identity.
void transformAttribute(PrTextBuffer& out, PrAffine2 t, PrStringView name = "transform") {
    if (prAffine2IsIdentity(t))
        return;
    prTextBufferAppendChar(&out, ' ');
    prTextBufferAppend(&out, name);
    prTextBufferAppend(&out, "=\"matrix(");
    float values[6] = {t.a, t.b, t.c, t.d, t.e, t.f};
    for (uint32_t i = 0; i < 6; ++i) {
        if (i != 0)
            prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, double(values[i]));
    }
    prTextBufferAppend(&out, ")\"");
}

void pathData(PrTextBuffer& out, const PrPath* path) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t p = 0;
    auto point = [&]() {
        drawSharedAppendNumber(out, double(points[p].x));
        prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, double(points[p].y));
        ++p;
    };
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
                prTextBufferAppendChar(&out, 'M');
                point();
                break;
            case PrPathVerb_Line:
                prTextBufferAppendChar(&out, 'L');
                point();
                break;
            case PrPathVerb_Quad:
                prTextBufferAppendChar(&out, 'Q');
                point();
                prTextBufferAppendChar(&out, ' ');
                point();
                break;
            case PrPathVerb_Cubic:
                prTextBufferAppendChar(&out, 'C');
                point();
                prTextBufferAppendChar(&out, ' ');
                point();
                prTextBufferAppendChar(&out, ' ');
                point();
                break;
            default:
                prTextBufferAppendChar(&out, 'Z');
                break;
        }
    }
}

// Appends a color as #rrggbb.
void color(PrTextBuffer& out, PrColor c) {
    PrColor clamped = drawSharedClamp(c);
    uint32_t r = uint32_t(clamped.r * 255 + 0.5f);
    uint32_t g = uint32_t(clamped.g * 255 + 0.5f);
    uint32_t b = uint32_t(clamped.b * 255 + 0.5f);
    prTextBufferAppendChar(&out, '#');
    drawSharedAppendHex(out, (r << 16) | (g << 8) | b, 3);
}

constexpr PrStringView kBlendModes[PrDrawBlendMode_Count] = {
    "normal", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn", "hard-light",
    "soft-light", "difference", "exclusion", "", "hue", "saturation", "color", "luminosity",
};

// Defines the gradient of `paint` and returns its id.
uint32_t defineGradient(SvgWriter& s, const DrawSharedPaint& paint) {
    const PrDrawBrush* brush = paint.brush;
    uint32_t id = nextId(s);
    PrTextBuffer& out = s.defs;
    if (brush->kind == PrDrawBrushKind_Linear) {
        prTextBufferAppend(&out, "<linearGradient");
        idAttribute(out, "id", 'g', id);
        numberAttribute(out, "x1", double(brush->linear.start.x));
        numberAttribute(out, "y1", double(brush->linear.start.y));
        numberAttribute(out, "x2", double(brush->linear.end.x));
        numberAttribute(out, "y2", double(brush->linear.end.y));
    } else {
        prTextBufferAppend(&out, "<radialGradient");
        idAttribute(out, "id", 'g', id);
        numberAttribute(out, "cx", double(brush->radial.end.x));
        numberAttribute(out, "cy", double(brush->radial.end.y));
        numberAttribute(out, "r", double(brush->radial.endRadius));
        numberAttribute(out, "fx", double(brush->radial.start.x));
        numberAttribute(out, "fy", double(brush->radial.start.y));
        numberAttribute(out, "fr", double(brush->radial.startRadius));
    }
    prTextBufferAppend(&out, " gradientUnits=\"userSpaceOnUse\"");
    transformAttribute(out, paint.brushToUser, "gradientTransform");
    if (brush->extend == PrDrawExtend_Repeat)
        prTextBufferAppend(&out, " spreadMethod=\"repeat\"");
    else if (brush->extend == PrDrawExtend_Reflect)
        prTextBufferAppend(&out, " spreadMethod=\"reflect\"");
    prTextBufferAppend(&out, ">");
    drawSharedGradientStops(brush, paint.color, s.stops);
    for (uint32_t i = 0; i < s.stops.count; ++i) {
        const DrawSharedStop& stop = s.stops.data[i];
        prTextBufferAppend(&out, "<stop");
        numberAttribute(out, "offset", double(stop.offset));
        prTextBufferAppend(&out, " stop-color=\"");
        color(out, stop.color);
        prTextBufferAppendChar(&out, '"');
        if (stop.color.a < 1)
            numberAttribute(out, "stop-opacity", double(stop.color.a));
        prTextBufferAppend(&out, "/>");
    }
    prTextBufferAppend(&out, brush->kind == PrDrawBrushKind_Linear ? "</linearGradient>" : "</radialGradient>");
    return id;
}

// Appends the attributes that paint `paint` as `property`, fill or stroke.
void paintAttributes(SvgWriter& s, PrStringView property, const DrawSharedPaint& paint) {
    PrTextBuffer& out = s.body;
    if (paint.brush != nullptr) {
        uint32_t gradient = defineGradient(s, paint);
        urlAttribute(out, property, 'g', gradient);
    } else {
        prTextBufferAppendChar(&out, ' ');
        prTextBufferAppend(&out, property);
        prTextBufferAppend(&out, "=\"");
        color(out, paint.color);
        prTextBufferAppendChar(&out, '"');
        float alpha = prF32Clamp(paint.color.a, 0, 1);
        if (alpha < 1) {
            prTextBufferAppendChar(&out, ' ');
            prTextBufferAppend(&out, property);
            prTextBufferAppend(&out, "-opacity=\"");
            drawSharedAppendNumber(out, double(alpha));
            prTextBufferAppendChar(&out, '"');
        }
    }
    if (paint.blendMode != PrDrawBlendMode_Normal) {
        prTextBufferAppend(&out, " style=\"mix-blend-mode:");
        prTextBufferAppend(&out, kBlendModes[paint.blendMode]);
        prTextBufferAppendChar(&out, '"');
    }
}

void strokeAttributes(PrTextBuffer& out, const PrDrawStroke* stroke) {
    const PrPathStroke& shape = stroke->shape;
    numberAttribute(out, "stroke-width", double(shape.width));
    if (shape.join == PrPathJoin_Round)
        prTextBufferAppend(&out, " stroke-linejoin=\"round\"");
    else if (shape.join == PrPathJoin_Bevel)
        prTextBufferAppend(&out, " stroke-linejoin=\"bevel\"");
    if (shape.join == PrPathJoin_Miter && shape.miterLimit != 4)
        numberAttribute(out, "stroke-miterlimit", double(prF32Max(shape.miterLimit, 1)));
    if (shape.startCap == PrPathCap_Round)
        prTextBufferAppend(&out, " stroke-linecap=\"round\"");
    else if (shape.startCap == PrPathCap_Square)
        prTextBufferAppend(&out, " stroke-linecap=\"square\"");
    if (shape.dashes != nullptr && shape.dashCount != 0) {
        prTextBufferAppend(&out, " stroke-dasharray=\"");
        for (uint32_t i = 0; i < shape.dashCount; ++i) {
            if (i != 0)
                prTextBufferAppendChar(&out, ' ');
            drawSharedAppendNumber(out, double(prF32Max(shape.dashes[i], 0)));
        }
        prTextBufferAppendChar(&out, '"');
        if (shape.dashOffset != 0)
            numberAttribute(out, "stroke-dashoffset", double(shape.dashOffset));
    }
    if (stroke->nonScaling)
        prTextBufferAppend(&out, " vector-effect=\"non-scaling-stroke\"");
}

bool svgGradient(void* PR_NULLABLE context, const PrDrawBrush* brush) {
    (void)context;
    if (brush->extend == PrDrawExtend_None)
        return false;
    if (brush->kind == PrDrawBrushKind_Linear)
        return true;
    if (brush->kind != PrDrawBrushKind_Radial)
        return false;
    // SVG's radial gradient draws a cone between two circles only where the start circle lies within the end circle.
    const PrDrawRadialGradient& r = brush->radial;
    float distance =
        prF32Sqrt((r.end.x - r.start.x) * (r.end.x - r.start.x) + (r.end.y - r.start.y) * (r.end.y - r.start.y));
    return r.startRadius >= 0 && distance + r.startRadius <= r.endRadius;
}

bool svgBlendMode(void* PR_NULLABLE context, PrDrawBlendMode blendMode) {
    (void)context;
    return blendMode != PrDrawBlendMode_HardMix;
}

void svgFill(void* PR_NULLABLE context, const PrPath* path, PrPathFillRule rule, PrAffine2 transform,
        const DrawSharedPaint* paint) {
    SvgWriter& s = *static_cast<SvgWriter*>(context);
    prTextBufferAppend(&s.body, "<path d=\"");
    pathData(s.body, path);
    prTextBufferAppendChar(&s.body, '"');
    transformAttribute(s.body, transform);
    paintAttributes(s, "fill", *paint);
    if (rule == PrPathFillRule_EvenOdd)
        prTextBufferAppend(&s.body, " fill-rule=\"evenodd\"");
    prTextBufferAppend(&s.body, "/>\n");
}

void svgStroke(void* PR_NULLABLE context, const PrPath* path, const PrDrawStroke* stroke, PrAffine2 transform,
        const DrawSharedPaint* paint) {
    SvgWriter& s = *static_cast<SvgWriter*>(context);
    prTextBufferAppend(&s.body, "<path d=\"");
    pathData(s.body, path);
    prTextBufferAppendChar(&s.body, '"');
    transformAttribute(s.body, transform);
    prTextBufferAppend(&s.body, " fill=\"none\"");
    paintAttributes(s, "stroke", *paint);
    strokeAttributes(s.body, stroke);
    prTextBufferAppend(&s.body, "/>\n");
}

void svgImage(void* PR_NULLABLE context, const PrDrawImage* image, PrRect source, PrRect destination,
        PrAffine2 transform, float opacity) {
    SvgWriter& s = *static_cast<SvgWriter*>(context);
    if (s.options.images == nullptr)
        return;
    PrUVec2 extent = prDrawImageExtent(image);
    drawSharedImageRgba8(image, s.rgba);
    PrPixelView pixels{s.rgba.data, size_t(extent.x) * 4, extent};
    size_t size = 0;
    if (prImageCodecEncode(s.options.images, "image/png", pixels, nullptr, 0, &size) != PrStatus_Ok || size == 0)
        return;
    RawArray<uint8_t> png;
    png.resize(uint32_t(size));
    prImageCodecEncode(s.options.images, "image/png", pixels, png.data, size, &size);
    prTextBufferResize(&s.encoded, uint32_t(prBase64EncodedSize(size)));
    size_t written = prBase64Encode(PrStringView{reinterpret_cast<const char*>(png.data), size}, s.encoded.data,
        s.encoded.size);
    png.reset();
    PrTextBuffer& out = s.body;
    // A nested svg element takes no transform, so a group around it does.
    bool transformed = !prAffine2IsIdentity(transform);
    if (transformed) {
        prTextBufferAppend(&out, "<g");
        transformAttribute(out, transform);
        prTextBufferAppend(&out, ">");
    }
    // The source's texels map onto the destination through a nested viewport, which cuts them to the source.
    prTextBufferAppend(&out, "<svg");
    numberAttribute(out, "x", double(destination.left));
    numberAttribute(out, "y", double(destination.top));
    numberAttribute(out, "width", double(destination.right - destination.left));
    numberAttribute(out, "height", double(destination.bottom - destination.top));
    prTextBufferAppend(&out, " viewBox=\"");
    drawSharedAppendNumber(out, double(source.left));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(source.top));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(source.right - source.left));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(source.bottom - source.top));
    prTextBufferAppend(&out, "\" preserveAspectRatio=\"none\" overflow=\"hidden\">");
    prTextBufferAppend(&out, "<image width=\"");
    prTextBufferAppendU64(&out, extent.x, 10);
    prTextBufferAppend(&out, "\" height=\"");
    prTextBufferAppendU64(&out, extent.y, 10);
    prTextBufferAppendChar(&out, '"');
    if (opacity < 1)
        numberAttribute(out, "opacity", double(prF32Clamp(opacity, 0, 1)));
    prTextBufferAppend(&out, " preserveAspectRatio=\"none\" href=\"data:image/png;base64,");
    prTextBufferAppend(&out, PrStringView{s.encoded.data, uint32_t(written)});
    prTextBufferAppend(&out, "\"/></svg>");
    prTextBufferAppend(&out, transformed ? "</g>\n" : "\n");
}

void svgGlyphs(void* PR_NULLABLE context, const PrFontGlyphRun* run, PrAffine2 transform,
        const PrDrawStroke* PR_NULLABLE stroke, const DrawSharedPaint* paint) {
    SvgWriter& s = *static_cast<SvgWriter*>(context);
    PrTextBuffer& out = s.body;
    prTextBufferAppend(&out, "<g");
    if (stroke == nullptr) {
        paintAttributes(s, "fill", *paint);
    } else {
        prTextBufferAppend(&out, " fill=\"none\"");
        paintAttributes(s, "stroke", *paint);
        strokeAttributes(out, stroke);
    }
    if (run->text.size != 0) {
        prTextBufferAppend(&out, " aria-label=\"");
        escaped(out, run->text);
        prTextBufferAppendChar(&out, '"');
    }
    prTextBufferAppend(&out, ">");
    uint64_t font = prFontId(run->font);
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        uint16_t glyph = run->glyphs[i].id;
        PrAffine2 placed = prAffine2Concat(prDrawGlyphTransform(run, i), transform);
        if (stroke != nullptr) {
            // A stroke is measured in the run's space, so each outline is placed there before it is stroked.
            prPathClear(s.path);
            if (!prDrawGlyphPath(run, i, nullptr, s.path))
                continue;
            prTextBufferAppend(&out, "<path d=\"");
            pathData(out, s.path);
            prTextBufferAppendChar(&out, '"');
            transformAttribute(out, transform);
            prTextBufferAppend(&out, "/>");
            continue;
        }
        uint32_t found = s.glyphIndex.find(glyphHash(font, glyph), [&](uint32_t at) {
            return s.glyphs.data[at].font == font && s.glyphs.data[at].id == glyph;
        });
        if (found == kHashMissing) {
            found = s.glyphs.count;
            prPathClear(s.path);
            bool outlined = prFontGlyphOutline(run->font, glyph, 0, prAffine2Identity(), s.path);
            s.glyphs.push(SvgGlyph{font, glyph, outlined});
            s.glyphIndex.insert(glyphHash(font, glyph), found);
            if (outlined) {
                prTextBufferAppend(&s.defs, "<path");
                idAttribute(s.defs, "id", 'f', found);
                prTextBufferAppend(&s.defs, " d=\"");
                pathData(s.defs, s.path);
                prTextBufferAppend(&s.defs, "\"/>\n");
            }
        }
        if (!s.glyphs.data[found].outlined)
            continue;
        prTextBufferAppend(&out, "<use href=\"#f");
        prTextBufferAppendU64(&out, found, 10);
        prTextBufferAppendChar(&out, '"');
        transformAttribute(out, placed);
        prTextBufferAppend(&out, "/>");
    }
    prTextBufferAppend(&out, "</g>\n");
}

void svgPushClip(void* PR_NULLABLE context, const PrPath* path, PrPathFillRule rule, PrAffine2 transform) {
    SvgWriter& s = *static_cast<SvgWriter*>(context);
    uint32_t id = nextId(s);
    prTextBufferAppend(&s.defs, "<clipPath");
    idAttribute(s.defs, "id", 'c', id);
    prTextBufferAppend(&s.defs, "><path d=\"");
    pathData(s.defs, path);
    prTextBufferAppendChar(&s.defs, '"');
    transformAttribute(s.defs, transform);
    if (rule == PrPathFillRule_EvenOdd)
        prTextBufferAppend(&s.defs, " clip-rule=\"evenodd\"");
    prTextBufferAppend(&s.defs, "/></clipPath>\n");
    prTextBufferAppend(&s.body, "<g");
    urlAttribute(s.body, "clip-path", 'c', id);
    prTextBufferAppend(&s.body, ">\n");
}

void svgPop(void* PR_NULLABLE context) {
    SvgWriter& s = *static_cast<SvgWriter*>(context);
    prTextBufferAppend(&s.body, "</g>\n");
}

void svgPushGroup(void* PR_NULLABLE context, const DrawSharedGroup* group) {
    SvgWriter& s = *static_cast<SvgWriter*>(context);
    prTextBufferAppend(&s.body, "<g");
    if (group->opacity < 1)
        numberAttribute(s.body, "opacity", double(group->opacity));
    if (group->blendMode != PrDrawBlendMode_Normal || group->isolated) {
        prTextBufferAppend(&s.body, " style=\"");
        if (group->blendMode != PrDrawBlendMode_Normal) {
            prTextBufferAppend(&s.body, "mix-blend-mode:");
            prTextBufferAppend(&s.body, kBlendModes[group->blendMode]);
            prTextBufferAppend(&s.body, ";");
        }
        if (group->isolated)
            prTextBufferAppend(&s.body, "isolation:isolate");
        prTextBufferAppendChar(&s.body, '"');
    }
    prTextBufferAppend(&s.body, ">\n");
}

void svgPushMark(void* PR_NULLABLE context, uint64_t key) {
    (void)context;
    (void)key;
}

void svgPopMark(void* PR_NULLABLE context, uint64_t key, PrRect bounds) {
    (void)context;
    (void)key;
    (void)bounds;
}

void destroyWriter(PrDrawWriter* writer) {
    SvgWriter* s = static_cast<SvgWriter*>(writer);
    drawSharedWalkerReset(s->walker);
    prTextBufferReset(&s->defs);
    prTextBufferReset(&s->body);
    prTextBufferReset(&s->scratch);
    s->rgba.reset();
    prTextBufferReset(&s->encoded);
    s->stops.reset();
    s->glyphs.reset();
    s->glyphIndex.reset();
    prPathDestroy(s->path);
    prMemoryFree(s);
}

void begin(PrDrawWriter* writer, const PrDrawDocumentDesc* desc) {
    SvgWriter& s = *static_cast<SvgWriter*>(writer);
    s.document = *desc;
    s.page = 0;
}

// Writes the page as a document of its own, of its sheet's extent, which defines what it uses, so each page's ids start
// again. SVG has no boxes of print production.
void addPage(PrDrawWriter* writer, const PrDrawPage* page) {
    SvgWriter& s = *static_cast<SvgWriter*>(writer);
    PrVec2 extent = page->extent;
    prTextBufferClear(&s.defs);
    prTextBufferClear(&s.body);
    s.glyphs.clear();
    s.glyphIndex.clear();
    s.nextId = 0;
    drawSharedWalk(s.walker, page->list, extent);
    PrTextBuffer& out = s.scratch;
    prTextBufferClear(&out);
    prTextBufferAppend(&out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"");
    numberAttribute(out, "width", double(extent.x));
    numberAttribute(out, "height", double(extent.y));
    prTextBufferAppend(&out, " viewBox=\"0 0 ");
    drawSharedAppendNumber(out, double(extent.x));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(extent.y));
    prTextBufferAppendChar(&out, '"');
    if (s.document.language.size != 0) {
        prTextBufferAppend(&out, " lang=\"");
        escaped(out, s.document.language);
        prTextBufferAppendChar(&out, '"');
    }
    prTextBufferAppend(&out, ">\n");
    if (s.document.title.size != 0) {
        prTextBufferAppend(&out, "<title>");
        escaped(out, s.document.title);
        prTextBufferAppend(&out, "</title>\n");
    }
    if (s.defs.size != 0) {
        prTextBufferAppend(&out, "<defs>\n");
        prTextBufferAppend(&out, PrStringView{s.defs.data, s.defs.size});
        prTextBufferAppend(&out, "</defs>\n");
    }
    uint32_t output = s.page++;
    s.document.write(s.document.context, output, prStringBytes(prTextBufferView(&out)));
    s.document.write(s.document.context, output, prStringBytes(prTextBufferView(&s.body)));
    s.document.write(s.document.context, output, PrByteView{reinterpret_cast<const uint8_t*>("</svg>\n"), 7});
}

void end(PrDrawWriter* writer) {
    (void)writer;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawWriter* prDrawSvgCreate(const PrDrawWriterOptions* PR_NULLABLE options) {
    SvgWriter* s = allocZeroed<SvgWriter>();
    s->destroyFn = destroyWriter;
    s->beginFn = begin;
    s->addPageFn = addPage;
    s->endFn = end;
    s->options = options != nullptr ? *options : prDrawWriterOptionsDefault();
    s->path = prPathCreate();
    DrawSharedWriterOps ops{
        .context = s,
        .knockout = false,
        .nonScalingStrokes = true,
        .gradient = svgGradient,
        .blendMode = svgBlendMode,
        .fill = svgFill,
        .stroke = svgStroke,
        .image = svgImage,
        .glyphs = svgGlyphs,
        .pushClip = svgPushClip,
        .popClip = svgPop,
        .pushGroup = svgPushGroup,
        .popGroup = svgPop,
        .pushMark = svgPushMark,
        .popMark = svgPopMark,
    };
    drawSharedWalkerInit(s->walker, ops, s->options.glyphDecoder, s->options.rasterScale);
    return s;
}
