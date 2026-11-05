// What the writers of documents share: the walk of a page's list that draws each command as natively as the format
// allows, lowers what it can, and rasterizes the rest with the CPU renderer, and the text that formats are written in.

#include "draw/backends/shared/src/draw_shared_writer.h"

#include "pr/draw_cpu.h"
#include "pr/draw_glyph.h"
#include "pr/draw_list.h"
#include "pr/draw_lower.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/integer.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// How far, in page px, a curve that a walk flattens may lie from the curve.
constexpr float kTolerance = 0.05f;

constexpr PrRect kEmpty{0, 0, 0, 0};

PrRect unionOf(PrRect a, PrRect b) {
    if (prRectIsEmpty(a))
        return b;
    if (prRectIsEmpty(b))
        return a;
    return prRectUnion(a, b);
}

// Returns true if a stroke has a form that a format draws natively (DrawSharedWriterOps::stroke).
bool strokeIsNative(const DrawSharedWalker& w, const PrDrawStroke* stroke) {
    const PrPathStroke& s = stroke->shape;
    bool dashed = s.dashes != nullptr && s.dashCount != 0;
    bool joins = s.join == PrPathJoin_Miter || s.join == PrPathJoin_Round || s.join == PrPathJoin_Bevel;
    bool caps = s.startCap == s.endCap && s.startCap != PrPathCap_Triangle && (!dashed || s.dashCap == s.startCap);
    return stroke->align == PrDrawStrokeAlign_Center && stroke->mapping == PrDrawStrokeMapping_Plain &&
        (s.profile == nullptr || s.profileCount == 0) && joins && caps &&
        (!stroke->nonScaling || w.ops.nonScalingStrokes);
}

// Returns true if every glyph of the run is an outline: none has color layers, an image or an SVG document.
bool glyphsAreOutlines(const PrFontGlyphRun* run) {
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        uint16_t id = run->glyphs[i].id;
        PrFontGlyphImage image;
        PrFontGlyphSvg svg;
        if (prFontGlyphHasColor(run->font, id) || prFontGlyphImage(run->font, id, run->size, &image) ||
                prFontGlyphSvg(run->font, id, &svg))
            return false;
    }
    return true;
}

// Returns the index of the pop that closes the clip pushed at `push`, or `count` if none does.
uint32_t clipEnd(const PrDrawCommand* commands, uint32_t count, uint32_t push) {
    uint32_t depth = 0;
    for (uint32_t i = push; i < count; ++i) {
        PrDrawCommandKind kind = commands[i].kind;
        if (kind >= PrDrawCommandKind_PushClipRect && kind <= PrDrawCommandKind_PushClipPath)
            ++depth;
        else if (kind == PrDrawCommandKind_PopClip && --depth == 0)
            return i;
    }
    return count;
}

// Returns the area of the page that the clips in effect let through.
PrRect clipBounds(const DrawSharedWalker& w) {
    return w.clips.count != 0 ? w.clips.last() : PrRect{0, 0, w.page.x, w.page.y};
}

// Opens a clip that lets through `bounds` of the page, within the clips in effect.
void pushClipBounds(DrawSharedWalker& w, PrRect bounds) {
    PrRect within = prRectIntersect(clipBounds(w), bounds);
    w.clips.push(prRectIsEmpty(within) ? kEmpty : within);
}

// Returns the walk's transform from the page's root to the commands that it is walking, placed in a render by
// `placement`, which maps the page to the render's pixels.
PrDrawTransform placedTransform(const DrawSharedWalker& w, PrAffine2 placement) {
    PrDrawTransform x = prDrawTransformFromAffine2(placement);
    for (uint32_t i = 0; i < w.steps.count; ++i) {
        const DrawSharedStep& step = w.steps.data[i];
        x = step.push != nullptr ? prDrawTransformPush(&x, step.push) : prDrawTransformMoved(&x, step.offset);
    }
    return x;
}

// Draws `count` commands of `list` from `first`, which close every scope that they open, as an image of what the CPU
// renderer draws for them under the walk's transform. The format's clips cut the image, so the render plays no clip.
// Returns the area of the page that the image covers, or an empty rect if the commands draw nothing.
PrRect rasterize(DrawSharedWalker& w, const PrDrawList* list, uint32_t first, uint32_t count, const PrDrawTransform& t) {
    PrRect local = prDrawCommandsBounds(prDrawListCommands(list) + first, count);
    if (prRectIsEmpty(local))
        return kEmpty;
    PrRect bounds = clipBounds(w);
    bool finite = prF32IsFinite(local.left) && prF32IsFinite(local.top) && prF32IsFinite(local.right) &&
        prF32IsFinite(local.bottom);
    if (finite)
        bounds = prRectIntersect(bounds, prAffine2MapRect(t.affine, local));
    bounds = prRectIntersect(bounds, PrRect{0, 0, w.page.x, w.page.y});
    if (prRectIsEmpty(bounds))
        return kEmpty;
    float scale = w.rasterScale;
    float side = prF32Max(bounds.right - bounds.left, bounds.bottom - bounds.top) * scale;
    if (side > float(kPrDrawRenderMaxSize))
        scale *= float(kPrDrawRenderMaxSize) / side;
    PrIRect area{
        int32_t(prF32Floor(bounds.left * scale)), int32_t(prF32Floor(bounds.top * scale)),
        int32_t(prF32Ceil(bounds.right * scale)), int32_t(prF32Ceil(bounds.bottom * scale)),
    };
    PrUVec2 extent{uint32_t(area.right - area.left), uint32_t(area.bottom - area.top)};
    if (extent.x == 0 || extent.y == 0)
        return kEmpty;
    PrAffine2 placement{scale, 0, 0, scale, -float(area.left), -float(area.top)};
    PrDrawPart part{.first = first, .count = count, .transform = placedTransform(w, placement)};
    size_t stride = size_t(extent.x) * 4;
    w.pixels.resizeZeroed(uint32_t(stride * extent.y));
    PrPixels pixels{w.pixels.data, stride, extent};
    if (prDrawRendererRenderPartPixels(w.cpu, list, &part, pixels, PrPixelFormat_Rgba8) != PrStatus_Ok)
        return kEmpty;
    PrDrawImageDesc desc{
        .pixels = PrPixelView{w.pixels.data, stride, extent},
        .format = PrPixelFormat_Rgba8,
        .alpha = PrDrawAlpha_Premultiplied,
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    if (prDrawImageCreate(&desc, &image) != PrStatus_Ok)
        return kEmpty;
    PrRect placed{
        float(area.left) / scale, float(area.top) / scale, float(area.right) / scale, float(area.bottom) / scale,
    };
    w.ops.image(w.ops.context, image, PrRect{0, 0, float(extent.x), float(extent.y)}, placed, prAffine2Identity(), 1);
    prDrawImageRelease(image);
    return placed;
}

// Returns the transform from a brush's space to the space of geometry that `toPage` maps to the page, for a command
// drawn under `transform` whose fill geometry has `bounds`. Returns false if the brush paints nothing.
bool brushToSpace(const PrDrawBrush* brush, PrAffine2 transform, PrRect bounds, PrAffine2 toPage, PrAffine2* out) {
    PrAffine2 pageToBrush;
    if (!prDrawBrushFromTarget(brush, transform, bounds, &pageToBrush))
        return false;
    // The geometry's space to the brush's, inverted.
    PrAffine2 spaceToBrush = prAffine2Concat(toPage, pageToBrush);
    return prAffine2Invert(spaceToBrush, out);
}

// The paint of a command, for geometry that `toPage` maps to the page, or false if the format must rasterize the
// command to paint it.
bool paintOf(DrawSharedWalker& w, const PrDrawCommand& c, const PrDrawTransform& t, PrAffine2 toPage, DrawSharedPaint* out) {
    *out = DrawSharedPaint{
        .color = prColorFromColor16(c.color),
        .brushToUser = prAffine2Identity(),
        .blendMode = c.blendMode,
    };
    if (c.brush == nullptr)
        return true;
    if (c.brush->kind == PrDrawBrushKind_Image || !w.ops.gradient(w.ops.context, c.brush))
        return false;
    out->brush = c.brush;
    return brushToSpace(c.brush, t.affine, prDrawFillBounds(&c), toPage, &out->brushToUser);
}

// The area of the page that a command may cover: its fill geometry, and the reach of its stroke.
PrRect pageBounds(const PrDrawCommand& c, const PrDrawTransform& t) {
    PrRect local = prDrawFillBounds(&c);
    if (prRectIsEmpty(local) && c.kind != PrDrawCommandKind_Line)
        return kEmpty;
    if (c.stroke != nullptr) {
        float reach = prDrawStrokeReach(c.stroke) * c.stroke->shape.width;
        if (!c.stroke->nonScaling)
            local = PrRect{local.left - reach, local.top - reach, local.right + reach, local.bottom + reach};
    }
    return prAffine2MapRect(t.affine, local);
}

struct BorderEmit {
    DrawSharedWalker* walker;
    const PrDrawCommand* command;
    PrAffine2 transform;
};

void emitBorderPiece(void* PR_NULLABLE context, const PrDrawBorderPiece* piece, const PrPathPolyline* shape,
        const PrPathPolyline* PR_NULLABLE cut) {
    BorderEmit& e = *static_cast<BorderEmit*>(context);
    DrawSharedWalker& w = *e.walker;
    const PrDrawBorderSide& side = e.command->border->sides[piece->side];
    DrawSharedPaint paint{
        .color = piece->color,
        .brush = side.brush,
        .brushToUser = prAffine2Identity(),
        .blendMode = e.command->blendMode,
    };
    if (side.brush != nullptr) {
        PrAffine2 toUser;
        if (!brushToSpace(side.brush, e.transform, e.command->border->shape.rect, e.transform, &toUser))
            return;
        paint.brushToUser = toUser;
    }
    if (cut != nullptr) {
        prPathClear(w.path);
        prPathAddPolyline(w.path, cut, prAffine2Identity());
        w.ops.pushClip(w.ops.context, w.path, PrPathFillRule_NonZero, e.transform);
    }
    prPathClear(w.path);
    prPathAddPolyline(w.path, shape, prAffine2Identity());
    w.ops.fill(w.ops.context, w.path, piece->rule, e.transform, &paint);
    if (cut != nullptr)
        w.ops.popClip(w.ops.context);
}

// Returns true if the border draws natively: each side that paints has no brush, or a gradient that the format draws.
bool borderIsNative(const DrawSharedWalker& w, const PrDrawBorder* border) {
    for (uint32_t i = 0; i < PrRectSide_Count; ++i) {
        const PrDrawBrush* brush = border->sides[i].brush;
        if (brush != nullptr && (brush->kind == PrDrawBrushKind_Image || !w.ops.gradient(w.ops.context, brush)))
            return false;
    }
    return true;
}

// Draws an image brush over the shape as the image clipped to the shape, if the brush is an image's texels placed once
// over a tile that holds the whole shape. Returns false if the brush is not such a brush.
bool drawImageBrush(DrawSharedWalker& w, const PrDrawCommand& c, const PrDrawTransform& t) {
    const PrDrawBrush* brush = c.brush;
    const PrDrawImageFill& fill = brush->image;
    if (c.stroke != nullptr || fill.source.kind != PrDrawSourceKind_Image || fill.source.image == nullptr)
        return false;
    PrAffine2 brushToUser;
    PrRect bounds = prDrawFillBounds(&c);
    if (!brushToSpace(brush, t.affine, bounds, t.affine, &brushToUser))
        return false;
    PrAffine2 userToBrush;
    if (!prAffine2Invert(brushToUser, &userToBrush))
        return false;
    // The shape must lie within the tile, so that nothing past the tile's edges shows. A small slack allows for
    // rounding where the shape is the tile.
    PrRect shape = prAffine2MapRect(userToBrush, bounds);
    PrRect tile = fill.tiles.tile;
    float slack = 1e-3f * prF32Max(tile.right - tile.left, tile.bottom - tile.top);
    if (shape.left < tile.left - slack || shape.top < tile.top - slack || shape.right > tile.right + slack ||
            shape.bottom > tile.bottom + slack)
        return false;
    prPathClear(w.path);
    prDrawShapePath(&c, w.path);
    w.ops.pushClip(w.ops.context, w.path, c.rule, t.affine);
    float opacity = prFloat16ToFloat(c.color.a);
    w.ops.image(w.ops.context, fill.source.image, fill.source.rect, tile, prAffine2Concat(brushToUser, t.affine), opacity);
    w.ops.popClip(w.ops.context);
    return true;
}

// Draws a painting command natively or lowered, and returns the area of the page that it covers. Returns false, and
// draws nothing, if the format must rasterize it.
bool drawPainting(DrawSharedWalker& w, const PrDrawCommand& command, const PrDrawTransform& t) {
    PrDrawSnapped snapped;
    const PrDrawCommand& c = *prDrawTransformSnap(&t, &command, &snapped);
    if (c.composite != PrDrawComposite_SourceOver || !w.ops.blendMode(w.ops.context, c.blendMode))
        return false;
    switch (c.kind) {
        case PrDrawCommandKind_Rect:
        case PrDrawCommandKind_CornerRect:
        case PrDrawCommandKind_Ellipse:
        case PrDrawCommandKind_Sector:
        case PrDrawCommandKind_Capsule:
        case PrDrawCommandKind_Line:
        case PrDrawCommandKind_Polyline:
        case PrDrawCommandKind_Path: {
            if (c.brush != nullptr && c.brush->kind == PrDrawBrushKind_Image)
                return drawImageBrush(w, c, t);
            prPathClear(w.path);
            prDrawShapePath(&c, w.path);
            if (c.stroke == nullptr) {
                DrawSharedPaint paint;
                if (!paintOf(w, c, t, t.affine, &paint))
                    return false;
                w.ops.fill(w.ops.context, w.path, c.rule, t.affine, &paint);
                return true;
            }
            if (strokeIsNative(w, c.stroke)) {
                DrawSharedPaint paint;
                if (!paintOf(w, c, t, t.affine, &paint))
                    return false;
                w.ops.stroke(w.ops.context, w.path, c.stroke, t.affine, &paint);
                return true;
            }
            // A centered stroke with plain mapping lowers into its outline on the page. Others rasterize.
            if (c.stroke->align != PrDrawStrokeAlign_Center || c.stroke->mapping != PrDrawStrokeMapping_Plain)
                return false;
            DrawSharedPaint paint;
            if (!paintOf(w, c, t, prAffine2Identity(), &paint))
                return false;
            prPathPolylineClear(&w.polyline);
            prPathPolylineClear(&w.aligned);
            prPathPolylineClear(&w.region);
            prDrawLowerStroke(w.path, c.stroke, t.affine, kTolerance, &w.polyline, &w.aligned, &w.region);
            prPathClear(w.path);
            prPathAddPolyline(w.path, &w.polyline, prAffine2Identity());
            w.ops.fill(w.ops.context, w.path, PrPathFillRule_NonZero, prAffine2Identity(), &paint);
            return true;
        }
        case PrDrawCommandKind_Border: {
            if (!borderIsNative(w, c.border))
                return false;
            BorderEmit emit{&w, &c, t.affine};
            float scale = prF32Max(prF32Sqrt(prF32Abs(t.affine.a * t.affine.d - t.affine.b * t.affine.c)), 1e-6f);
            prDrawBorderLower(c.border, kTolerance / scale, emitBorderPiece, &emit);
            return true;
        }
        case PrDrawCommandKind_Decoration: {
            DrawSharedPaint paint;
            if (c.brush != nullptr && c.brush->kind == PrDrawBrushKind_Image)
                return false;
            if (!paintOf(w, c, t, t.affine, &paint))
                return false;
            float scale = prF32Max(prF32Sqrt(prF32Abs(t.affine.a * t.affine.d - t.affine.b * t.affine.c)), 1e-6f);
            prPathPolylineClear(&w.polyline);
            prDrawDecorationOutline(c.decoration, kTolerance / scale, &w.polyline);
            prPathClear(w.path);
            prPathAddPolyline(w.path, &w.polyline, prAffine2Identity());
            w.ops.fill(w.ops.context, w.path, PrPathFillRule_NonZero, t.affine, &paint);
            return true;
        }
        case PrDrawCommandKind_Glyphs: {
            if (c.brush != nullptr && c.brush->kind == PrDrawBrushKind_Image)
                return false;
            if (c.stroke != nullptr && !strokeIsNative(w, c.stroke))
                return false;
            if (!glyphsAreOutlines(c.glyphs))
                return false;
            DrawSharedPaint paint;
            if (!paintOf(w, c, t, t.affine, &paint))
                return false;
            w.ops.glyphs(w.ops.context, c.glyphs, t.affine, c.stroke, &paint);
            return true;
        }
        default:
            // Clears, shadows, nine-slices and written pixels have no native form in a document.
            return false;
    }
}

// Returns true if a group draws natively: its only effects are opacity, a blend mode the format has, isolation and
// knockout if the format has it.
bool groupIsNative(const DrawSharedWalker& w, const PrDrawGroup* g) {
    return g->filter == nullptr && g->backdrop == nullptr && g->mask == nullptr && g->projection == nullptr &&
        g->composite == PrDrawComposite_SourceOver && w.ops.blendMode(w.ops.context, g->blendMode) &&
        (!g->knockout || w.ops.knockout);
}

// Returns the key of the first mark among the commands, or among those of the lists that they embed, or zero.
uint64_t firstMarkKey(const PrDrawCommand* commands, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (commands[i].kind == PrDrawCommandKind_PushMark)
            return commands[i].key;
        if (commands[i].kind == PrDrawCommandKind_Embed) {
            const PrDrawList* list = commands[i].embed.list;
            if (uint64_t key = firstMarkKey(prDrawListCommands(list), prDrawListCommandCount(list)))
                return key;
        }
    }
    return 0;
}

void addToMark(DrawSharedWalker& w, PrRect bounds) {
    if (w.marks.count != 0)
        w.marks.last().bounds = unionOf(w.marks.last().bounds, bounds);
}

void walkList(DrawSharedWalker& w, const PrDrawList* list, PrDrawTransform base) {
    const PrDrawCommand* commands = prDrawListCommands(list);
    uint32_t count = prDrawListCommandCount(list);
    uint32_t floor = w.transforms.count;
    w.transforms.push(base);
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = commands[i];
        const PrDrawTransform t = w.transforms.last();
        if (prDrawTransformDrawsNothing(&t, &c)) {
            // A clip or a group that draws nothing still closes with its pop, which the walk skips with it.
            if (c.kind >= PrDrawCommandKind_PushClipRect && c.kind <= PrDrawCommandKind_PushClipPath)
                i = clipEnd(commands, count, i);
            else if (c.kind == PrDrawCommandKind_PushGroup)
                i = prDrawCommandGroupEnd(commands, count, i);
            continue;
        }
        switch (c.kind) {
            case PrDrawCommandKind_PushTransformAffine2:
                w.transforms.push(prDrawTransformPush(&t, &c));
                w.steps.push(DrawSharedStep{.push = &c});
                break;
            case PrDrawCommandKind_PopTransform:
                w.transforms.pop();
                w.steps.pop();
                break;
            case PrDrawCommandKind_PushClipRect:
            case PrDrawCommandKind_PushClipCornerRect:
            case PrDrawCommandKind_PushClipPath: {
                PrDrawSnapped snapped;
                const PrDrawCommand& clip = *prDrawTransformSnap(&t, &c, &snapped);
                prPathClear(w.path);
                prDrawShapePath(&clip, w.path);
                w.ops.pushClip(w.ops.context, w.path, clip.rule, t.affine);
                pushClipBounds(w, prAffine2MapRect(t.affine, prPathTightBounds(w.path)));
                break;
            }
            case PrDrawCommandKind_PushClipTargetRect:
                prPathClear(w.path);
                prPathAddRect(w.path, c.rect);
                w.ops.pushClip(w.ops.context, w.path, PrPathFillRule_NonZero, prAffine2Identity());
                pushClipBounds(w, c.rect);
                break;
            case PrDrawCommandKind_PopClip:
                w.ops.popClip(w.ops.context);
                w.clips.pop();
                break;
            case PrDrawCommandKind_PushGroup: {
                const PrDrawGroup* g = c.group;
                uint32_t end = prDrawCommandGroupEnd(commands, count, i);
                if (!groupIsNative(w, g)) {
                    // What the group holds becomes one image, which takes the place of its first mark's content.
                    uint64_t key = firstMarkKey(commands + i, end + 1 - i);
                    if (key != 0) {
                        w.ops.pushMark(w.ops.context, key);
                        w.marks.push(DrawSharedMark{key, kEmpty});
                    }
                    PrRect drawn = rasterize(w, list, i, end + 1 - i, t);
                    if (key != 0) {
                        w.marks.pop();
                        w.ops.popMark(w.ops.context, key, drawn);
                    }
                    addToMark(w, drawn);
                    i = end;
                    break;
                }
                PrRect bounds{0, 0, w.page.x, w.page.y};
                if (!prRectIsEmpty(g->bounds))
                    bounds = prAffine2MapRect(t.affine, g->bounds);
                DrawSharedGroup group{
                    .opacity = prF32Clamp(g->opacity, 0, 1),
                    .blendMode = g->blendMode,
                    .isolated = g->isolated,
                    .knockout = g->knockout,
                    .bounds = prRectIntersect(bounds, PrRect{0, 0, w.page.x, w.page.y}),
                };
                w.ops.pushGroup(w.ops.context, &group);
                break;
            }
            case PrDrawCommandKind_PopGroup:
                w.ops.popGroup(w.ops.context);
                break;
            case PrDrawCommandKind_PushMark:
                w.ops.pushMark(w.ops.context, c.key);
                w.marks.push(DrawSharedMark{c.key, kEmpty});
                break;
            case PrDrawCommandKind_PopMark: {
                DrawSharedMark mark = w.marks.last();
                w.marks.pop();
                addToMark(w, mark.bounds);
                w.ops.popMark(w.ops.context, mark.key, mark.bounds);
                break;
            }
            case PrDrawCommandKind_Embed: {
                // An embedded list outside the clips, as the lists of other pages are, is not walked.
                PrDrawTransform moved = prDrawTransformMoved(&t, c.embed.offset);
                PrRect within = prRectIntersect(clipBounds(w), PrRect{0, 0, w.page.x, w.page.y});
                if (prDrawListMisses(c.embed.list, moved.affine, within))
                    break;
                w.steps.push(DrawSharedStep{.offset = c.embed.offset});
                walkList(w, c.embed.list, moved);
                w.steps.pop();
                break;
            }
            default:
                if (drawPainting(w, c, t))
                    addToMark(w, pageBounds(c, t));
                else
                    addToMark(w, rasterize(w, list, i, 1, t));
                break;
        }
    }
    w.transforms.resize(floor);
}

} // namespace

void drawSharedAppendNumber(PrTextBuffer& out, double value, uint32_t decimals) {
    prTextBufferAppendF64Fixed(&out, drawSharedRound(value, decimals), decimals);
}

double drawSharedRound(double value, uint32_t decimals) {
    if (!prF64IsFinite(value))
        return 0;
    uint64_t scale = 1;
    for (uint32_t i = 0; i < decimals; ++i)
        scale *= 10;
    bool negative = value < 0;
    double magnitude = negative ? -value : value;
    // Past what 64 bits hold at this precision, the number keeps its integer part.
    if (magnitude * double(scale) >= 9.0e18) {
        scale = 1;
        if (magnitude >= 9.0e18)
            magnitude = 9.0e18;
    }
    double rounded = double(uint64_t(magnitude * double(scale) + 0.5)) / double(scale);
    return negative ? -rounded : rounded;
}

void drawSharedAppendHex(PrTextBuffer& out, uint64_t value, uint32_t bytes) {
    char digits[kPrU64TextMaxBytes];
    uint32_t count = uint32_t(prU64Format(value, digits, 16));
    for (uint32_t i = count; i < bytes * 2; ++i)
        prTextBufferAppendChar(&out, '0');
    prAsciiUpperText(digits, count);
    prTextBufferAppend(&out, PrStringView{digits, count});
}

PrColor drawSharedClamp(PrColor color) {
    return PrColor{
        prF32Clamp(color.r, 0, 1), prF32Clamp(color.g, 0, 1), prF32Clamp(color.b, 0, 1), prF32Clamp(color.a, 0, 1),
    };
}

bool drawSharedImageRgba8(const PrDrawImage* image, RawArray<uint8_t>& out) {
    PrUVec2 extent = prDrawImageExtent(image);
    PrPixelView pixels = prDrawImagePixels(image);
    PrPixelFormat format = prDrawImageFormat(image);
    PrDrawAlpha alpha = prDrawImageAlpha(image);
    out.resize(extent.x * extent.y * 4);
    bool translucent = false;
    for (uint32_t y = 0; y < extent.y; ++y) {
        const uint8_t* row = pixels.data + y * pixels.stride;
        for (uint32_t x = 0; x < extent.x; ++x) {
            float c[4];
            if (format == PrPixelFormat_RgbaF16) {
                PrFloat16 wide[4];
                prMemCopy(wide, row + x * 8, sizeof(wide));
                for (uint32_t k = 0; k < 4; ++k)
                    c[k] = prFloat16ToFloat(wide[k]);
            } else {
                const uint8_t* p = row + x * 4;
                bool bgra = format == PrPixelFormat_Bgra8;
                c[0] = float(p[bgra ? 2 : 0]) / 255;
                c[1] = float(p[1]) / 255;
                c[2] = float(p[bgra ? 0 : 2]) / 255;
                c[3] = float(p[3]) / 255;
            }
            if (alpha == PrDrawAlpha_Opaque)
                c[3] = 1;
            c[3] = prF32Clamp(c[3], 0, 1);
            if (alpha == PrDrawAlpha_Premultiplied && c[3] > 0) {
                for (uint32_t k = 0; k < 3; ++k)
                    c[k] /= c[3];
            }
            uint8_t* o = out.data + (y * extent.x + x) * 4;
            for (uint32_t k = 0; k < 4; ++k)
                o[k] = uint8_t(prF32Clamp(c[k], 0, 1) * 255 + 0.5f);
            translucent = translucent || o[3] != 255;
        }
    }
    return translucent;
}

void drawSharedGradientStops(const PrDrawBrush* brush, PrColor color, RawArray<DrawSharedStop>& out) {
    out.clear();
    // Pieces that mix in another space or around a hue come as colors at every 1/64 of the parameter.
    constexpr uint32_t kResolution = 64;
    // A piece whose ends differ in alpha mixes premultiplied, which straight stops reach only through steps between
    // them.
    constexpr uint32_t kAlphaSteps = 16;
    RawArray<PrDrawRampPiece> pieces;
    RawArray<PrColor> colors;
    pieces.resize(prDrawGradientRampMaxPieces(brush->stopCount));
    colors.resize(prDrawGradientRampMaxColors(brush->stopCount, kResolution));
    uint32_t count = prDrawGradientRamp(brush, color, PrColorSpace_Srgb, kResolution, pieces.data, colors.data);
    auto add = [&](float offset, PrColor premultiplied) {
        PrColor c = premultiplied;
        if (c.a > 0) {
            c.r /= c.a;
            c.g /= c.a;
            c.b /= c.a;
        }
        DrawSharedStop stop{prF32Clamp(offset, 0, 1), drawSharedClamp(c)};
        // A stop that repeats the last one adds nothing, and of three stops at one offset the middle one shows nowhere.
        if (out.count != 0 && out.last().offset == stop.offset &&
                prMemEquals(&out.last().color, &stop.color, sizeof(PrColor)))
            return;
        if (out.count >= 2 && out.last().offset == stop.offset && out.data[out.count - 2].offset == stop.offset)
            out.pop();
        out.push(stop);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawRampPiece& piece = pieces.data[i];
        float start = prF32Max(piece.start, 0);
        float end = prF32Min(piece.end, 1);
        if (start > end || (start == end && (piece.end < 0 || piece.start > 1)))
            continue;
        const PrColor* c = colors.data + piece.first;
        uint32_t steps = piece.count > 1 ? piece.count - 1 : 1;
        if (piece.count == 2 && c[0].a != c[1].a)
            steps = kAlphaSteps;
        for (uint32_t k = 0; k <= steps; ++k) {
            float t = start + (end - start) * float(k) / float(steps);
            add(t, prDrawRampColor(&piece, colors.data, t));
        }
    }
    if (out.count == 0)
        add(0, PrColor{0, 0, 0, 0});
    pieces.reset();
    colors.reset();
}

void drawSharedWalkerInit(DrawSharedWalker& walker, const DrawSharedWriterOps& ops,
        PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder, float rasterScale) {
    walker = DrawSharedWalker{};
    walker.ops = ops;
    PrDrawCpuOptions cpu = prDrawCpuOptionsDefault();
    cpu.glyphDecoder = glyphDecoder;
    walker.cpu = prDrawCpuCreate(&cpu);
    walker.rasterScale = rasterScale;
    walker.path = prPathCreate();
}

void drawSharedWalkerReset(DrawSharedWalker& walker) {
    prDrawRendererDestroy(walker.cpu);
    prPathDestroy(walker.path);
    prPathPolylineReset(&walker.polyline);
    prPathPolylineReset(&walker.aligned);
    prPathPolylineReset(&walker.region);
    walker.steps.reset();
    walker.clips.reset();
    walker.transforms.reset();
    walker.marks.reset();
    walker.pixels.reset();
}

void drawSharedWalk(DrawSharedWalker& walker, const PrDrawList* list, PrVec2 extent) {
    walker.page = extent;
    walker.steps.clear();
    walker.clips.clear();
    walker.transforms.clear();
    walker.marks.clear();
    walkList(walker, list, prDrawTransformFromAffine2(prAffine2Identity()));
}

} // namespace pr_impl
