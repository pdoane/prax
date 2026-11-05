// Draw lists: recorded commands that a backend renders.

#include "pr/draw_list.h"
#include "draw/src/draw_list.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/bitset.h"
#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_projection.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "draw/src/identity.h"

namespace pr_impl {

namespace {

// The size in bytes of a block of side storage. The data that commands point to lives there without moving.
constexpr size_t kBlockBytes = 4096;

constexpr PrRect kUnbounded{-kPrF32Infinity, -kPrF32Infinity, kPrF32Infinity, kPrF32Infinity};

// Allocates `size` bytes of side storage, aligned for anything that a command points to.
void* sideAlloc(DrawRecording& r, size_t size) {
    size = (size + 15) & ~size_t(15);
    if (r.blocks.count == 0 || r.blockUsed + size > r.blockSize) {
        size_t blockSize = size > kBlockBytes ? size : kBlockBytes;
        r.blocks.push(static_cast<uint8_t*>(prMemoryAlloc(blockSize)));
        r.blockSize = blockSize;
        r.blockUsed = 0;
    }
    void* at = r.blocks.last() + r.blockUsed;
    r.blockUsed += size;
    return at;
}

template <typename T>
T* sideCopy(DrawRecording& r, const T* items, uint32_t count) {
    T* copy = static_cast<T*>(sideAlloc(r, sizeof(T) * count));
    prMemCopy(copy, items, sizeof(T) * count);
    return copy;
}

// Empties the recording. Releases its references and keeps the storage of its arrays.
void clearRecording(DrawRecording& r) {
    for (uint32_t i = 0; i < r.paths.count; ++i)
        prPathDestroy(r.paths.data[i]);
    for (uint32_t i = 0; i < r.images.count; ++i)
        prDrawImageRelease(r.images.data[i]);
    for (uint32_t i = 0; i < r.fonts.count; ++i)
        prFontRelease(r.fonts.data[i]);
    for (uint32_t i = 0; i < r.lists.count; ++i)
        drawListRelease(r.lists.data[i]);
    for (uint32_t i = 0; i < r.filters.count; ++i)
        prDrawFilterRelease(r.filters.data[i]);
    for (uint32_t i = 0; i < r.held.count; ++i)
        r.held.data[i].release(r.held.data[i].context);
    for (uint32_t i = 0; i < r.blocks.count; ++i)
        prMemoryFree(r.blocks.data[i]);
    r.commands.clear();
    r.blocks.clear();
    r.paths.clear();
    r.images.clear();
    r.fonts.clear();
    r.lists.clear();
    r.filters.clear();
    r.held.clear();
    r.blockUsed = 0;
    r.blockSize = 0;
}

void resetRecording(DrawRecording& r) {
    clearRecording(r);
    r.commands.reset();
    r.blocks.reset();
    r.paths.reset();
    r.images.reset();
    r.fonts.reset();
    r.lists.reset();
    r.filters.reset();
    r.held.reset();
}

const PrPath* holdPath(DrawRecording& r, const PrPath* path) {
    PrPath* clone = prPathClone(path);
    r.paths.push(clone);
    return clone;
}

void holdImage(DrawRecording& r, const PrDrawImage* image) {
    prDrawImageRetain(image);
    r.images.push(image);
}

void holdFont(DrawRecording& r, const PrFont* font) {
    prFontRetain(font);
    r.fonts.push(font);
}

PrRect recordingBounds(const DrawRecording& r);

// Takes a reference to another list, which is frozen.
void holdList(DrawRecording& r, const PrDrawList* list) {
    PR_ASSERT(list->frozen);
    drawListRetain(list);
    r.lists.push(list);
}

void holdFilter(DrawRecording& r, const PrDrawFilter* filter) {
    prDrawFilterRetain(filter);
    r.filters.push(filter);
}

// Takes a reference to the source's image or list.
void holdSource(DrawRecording& r, const PrDrawSource& source) {
    if (source.kind == PrDrawSourceKind_Image && source.image != nullptr)
        holdImage(r, source.image);
    else if (source.kind == PrDrawSourceKind_List && source.list != nullptr)
        holdList(r, source.list);
}

// Copies the brush's stops into side storage and takes a reference to its image or list.
void holdBrush(DrawRecording& r, PrDrawBrush& brush) {
    for (uint32_t i = 0; brush.stops != nullptr && i < brush.stopCount; ++i)
        PR_ASSERT(brush.stops[i].hint >= 0 && brush.stops[i].hint < 1);
    if (brush.stops != nullptr && brush.stopCount != 0)
        brush.stops = sideCopy(r, brush.stops, brush.stopCount);
    else
        brush.stopCount = 0;
    if (brush.kind == PrDrawBrushKind_Image)
        holdSource(r, brush.image.source);
}

const PrDrawBrush* copyBrush(DrawRecording& r, const PrDrawBrush* brush) {
    PrDrawBrush* copy = sideCopy(r, brush, 1);
    holdBrush(r, *copy);
    return copy;
}

// Returns true if two brushes have the same fields and the same stops. A brush has no padding, so its bytes compare.
bool brushesEqual(const PrDrawBrush& a, const PrDrawBrush& b) {
    uint32_t count = a.stops != nullptr ? a.stopCount : 0;
    if (count != (b.stops != nullptr ? b.stopCount : 0))
        return false;
    PrDrawBrush bWithStops = b;
    bWithStops.stops = a.stops;
    bWithStops.stopCount = a.stopCount;
    return prMemEquals(&a, &bWithStops, sizeof(PrDrawBrush)) &&
        (count == 0 || prMemEquals(a.stops, b.stops, sizeof(PrDrawColorStop) * count));
}

// Drops the brush's source if it is the list that the brush is recorded into. Such a list would draw itself forever.
void withoutSelf(const DrawList* list, PrDrawBrush& brush) {
    if (brush.kind == PrDrawBrushKind_Image && brush.image.source.kind == PrDrawSourceKind_List &&
            brush.image.source.list == list)
        brush.image.source.list = nullptr;
}

// Returns true if a brush paints anything: a gradient with stops, or an image brush with a valid source.
bool brushPaints(const PrDrawBrush* brush) {
    if (brush->kind == PrDrawBrushKind_Image)
        return prDrawSourceIsValid(brush->image.source);
    return brush->stops != nullptr && brush->stopCount != 0;
}

// Returns true if two strokes have the same shape, measure, alignment and mapping.
bool strokesEqual(const PrDrawStroke& a, const PrDrawStroke& b) {
    const PrPathStroke& x = a.shape;
    const PrPathStroke& y = b.shape;
    uint32_t dashes = x.dashes != nullptr ? x.dashCount : 0;
    uint32_t profile = x.profile != nullptr ? x.profileCount : 0;
    return a.nonScaling == b.nonScaling && a.align == b.align && a.mapping == b.mapping && x.width == y.width &&
        x.miterLimit == y.miterLimit && x.dashOffset == y.dashOffset && x.join == y.join &&
        x.startCap == y.startCap && x.endCap == y.endCap && x.dashCap == y.dashCap &&
        x.zeroLengthDirection.x == y.zeroLengthDirection.x && x.zeroLengthDirection.y == y.zeroLengthDirection.y &&
        dashes == (y.dashes != nullptr ? y.dashCount : 0) && profile == (y.profile != nullptr ? y.profileCount : 0) &&
        (dashes == 0 || prMemEquals(x.dashes, y.dashes, sizeof(float) * dashes)) &&
        (profile == 0 || prMemEquals(x.profile, y.profile, sizeof(PrPathWidthPoint) * profile));
}

const PrDrawStroke* copyStroke(DrawRecording& r, const PrDrawStroke* stroke) {
    PrDrawStroke* copy = sideCopy(r, stroke, 1);
    PrPathStroke& shape = copy->shape;
    if (shape.dashes != nullptr && shape.dashCount != 0) {
        shape.dashes = sideCopy(r, shape.dashes, shape.dashCount);
    } else {
        shape.dashes = nullptr;
        shape.dashCount = 0;
    }
    if (shape.profile != nullptr && shape.profileCount != 0) {
        shape.profile = sideCopy(r, shape.profile, shape.profileCount);
    } else {
        shape.profile = nullptr;
        shape.profileCount = 0;
    }
    return copy;
}

// Every change to a list gives it a new id.
void changed(DrawList* list) {
    PR_ASSERT(!list->frozen);
    list->id = 0;
}

PrDrawCommand* add(DrawList* list, PrDrawCommandKind kind, PrDrawEdges edges) {
    changed(list);
    DrawRecording& r = list->recording;
    PrDrawCommand* command = &r.commands.emplace();
    prMemZero(command, sizeof(PrDrawCommand));
    command->kind = kind;
    command->antialias = edges != PrDrawEdges_Aliased;
    command->snap = edges == PrDrawEdges_Snapped ? PrDrawSnapFlag_X | PrDrawSnapFlag_Y : 0;
    return command;
}

// Returns the list's copy of `brush`: the last copy if the brush equals it, and otherwise a new copy.
const PrDrawBrush* recordBrush(DrawList* list, const PrDrawBrush& brush) {
    if (list->lastBrush == nullptr || !brushesEqual(*list->lastBrush, brush))
        list->lastBrush = copyBrush(list->recording, &brush);
    return list->lastBrush;
}

// Returns the list's copy of `stroke`: the last copy if the stroke equals it, and otherwise a new copy.
const PrDrawStroke* recordStroke(DrawList* list, const PrDrawStroke& stroke) {
    if (list->lastStroke == nullptr || !strokesEqual(*list->lastStroke, stroke))
        list->lastStroke = copyStroke(list->recording, &stroke);
    return list->lastStroke;
}

// Adds a painting command in `paint`, or returns null if the command cannot paint: its color has no alpha, or its brush
// paints nothing. The command's color is the paint's color, or white at the brush's opacity.
PrDrawCommand* PR_NULLABLE addPainting(DrawList* list, PrDrawCommandKind kind, const PrDrawPaint& paint) {
    PrColor16 color = paint.color;
    PrDrawBrush brush;
    if (paint.brush != nullptr) {
        brush = *paint.brush;
        withoutSelf(list, brush);
        if (!brushPaints(&brush))
            return nullptr;
        color = PR_COLOR16(1, 1, 1, brush.opacity);
    }
    if (!(prFloat16ToFloat(color.a) > 0))
        return nullptr;
    PrDrawCommand* command = add(list, kind, paint.edges);
    command->composite = paint.composite;
    command->blendMode = paint.blendMode;
    command->color = color;
    if (paint.brush != nullptr)
        command->brush = recordBrush(list, brush);
    return command;
}

// Adds a shape in `paint` if it can paint, or returns null. A fill needs area, and a stroke needs a width above zero.
PrDrawCommand* PR_NULLABLE addShape(DrawList* list, PrDrawCommandKind kind, bool hasArea, const PrDrawPaint& paint) {
    if (paint.stroke == nullptr ? !hasArea : !(paint.stroke->shape.width > 0))
        return nullptr;
    PrDrawCommand* command = addPainting(list, kind, paint);
    if (command != nullptr && paint.stroke != nullptr)
        command->stroke = recordStroke(list, *paint.stroke);
    return command;
}

PrDrawCommand* push(DrawList* list, PrDrawCommandKind kind, PrDrawCommandKind group, PrDrawEdges edges) {
    list->open.push(group);
    return add(list, kind, edges);
}

// Closes the most recent open push, which is of the kind `push`. A pop that closes no such push records nothing.
void pop(DrawList* list, PrDrawCommandKind push, PrDrawCommandKind kind) {
    PR_ASSERT(list->open.count != 0 && list->open.last() == push);
    if (list->open.count == 0 || list->open.last() != push)
        return;
    list->open.pop();
    add(list, kind, PrDrawEdges_Antialiased);
}

// ---------------------------------------------------------------------------------------------------------------------
// Bounds

PrRect outset(PrRect r, float by) {
    return PrRect{r.left - by, r.top - by, r.right + by, r.bottom + by};
}

bool unbounded(PrRect r) {
    const float infinity = kPrF32Infinity;
    return r.left == -infinity || r.top == -infinity || r.right == infinity || r.bottom == infinity;
}

PrRect mapBounds(PrAffine2 t, PrRect r) {
    if (prRectIsEmpty(r) || unbounded(r))
        return r;
    if (!prF32IsFinite(t.a))
        return kUnbounded;
    return prAffine2MapRect(t, r);
}

// Computes how far a stroke may extend past its geometry. The reach is in the stroke's own space, or in target pixels
// if the stroke is measured in target pixels.
void strokeReach(const PrDrawStroke* stroke, float* local, float* device) {
    float reach = prDrawStrokeReach(stroke) * stroke->shape.width;
    *local = stroke->nonScaling ? 0 : reach;
    *device = stroke->nonScaling ? reach : 0;
}

// Returns the area that a glyph run may cover: each glyph's outline and its cell, which color layers may reach. Returns
// the box of `r` mapped through `t`. A transform that only scales and moves maps a box to a box.
PrRect mappedBox(PrAffine2 t, PrRect r) {
    if (t.b != 0 || t.c != 0)
        return prAffine2MapRect(t, r);
    float x0 = r.left * t.a + t.e;
    float x1 = r.right * t.a + t.e;
    float y0 = r.top * t.d + t.f;
    float y1 = r.bottom * t.d + t.f;
    return PrRect{prF32Min(x0, x1), prF32Min(y0, y1), prF32Max(x0, x1), prF32Max(y0, y1)};
}

PrRect glyphRunBounds(const PrFontGlyphRun* run) {
    PrFontMetrics metrics = prFontMetrics(run->font);
    // Glyphs without transforms of their own share the first glyph's transform, moved to each glyph's position.
    bool shared = run->transforms == nullptr && run->glyphCount != 0;
    PrAffine2 base = shared ? prDrawGlyphTransform(run, 0) : prAffine2Identity();
    PrRect bounds{0, 0, 0, 0};
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        const PrFontGlyph& g = run->glyphs[i];
        float advance = prFontGlyphAdvance(run->font, g.id, 0);
        PrRect cell{0, -metrics.ascent, advance, metrics.descent};
        PrRect glyph = prRectUnion(cell, prFontGlyphBounds(run->font, g.id));
        PrAffine2 t = base;
        if (shared) {
            t.e = g.position.x;
            t.f = g.position.y;
        } else {
            t = prDrawGlyphTransform(run, i);
        }
        bounds = prRectUnion(bounds, mappedBox(t, glyph));
    }
    return bounds;
}

// Returns the bounds of commands as they play, using the transforms and clips that they push. The commands close every
// scope that they open.
PrRect commandsBounds(const PrDrawCommand* commands, uint32_t count) {
    RawArray<PrDAffine2> transforms;
    RawArray<PrRect> clips;
    transforms.push(prDAffine2Identity());
    clips.push(kUnbounded);
    PrRect bounds{0, 0, 0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = commands[i];
        // Transforms compose in doubles, and map bounds in floats.
        PrAffine2 t = prAffine2FromDAffine2(transforms.last());
        PrRect clip = clips.last();
        PrRect local{0, 0, 0, 0};
        float device = 0;
        bool painting = true;
        switch (c.kind) {
            case PrDrawCommandKind_Clear:
                local = kUnbounded;
                break;
            case PrDrawCommandKind_WritePixels: {
                // Placed in target pixels, regardless of the transforms and clips.
                PrIRect box = prIRectAt(c.writePixels.origin, prDrawImageExtent(c.writePixels.image));
                bounds = prRectUnion(bounds, prRectFromIRect(box));
                painting = false;
                break;
            }
            case PrDrawCommandKind_Border:
                local = c.border->shape.rect;
                break;
            case PrDrawCommandKind_DropShadow: {
                const PrDrawShadow* s = c.shadow;
                const PrRect& rect = s->shape.rect;
                PrRect caster = PrRect{
                    rect.left + s->offset.x, rect.top + s->offset.y, rect.right + s->offset.x,
                    rect.bottom + s->offset.y,
                };
                float reach = prF32Max(s->spread, 0) + 4 * prF32Max(s->sigma, 0);
                if (s->nonScaling) {
                    local = rect;
                    device = reach + prF32Abs(s->offset.x) + prF32Abs(s->offset.y);
                } else {
                    local = outset(prRectUnion(caster, rect), reach);
                }
                break;
            }
            case PrDrawCommandKind_InnerShadow:
                local = c.shadow->shape.rect;
                break;
            case PrDrawCommandKind_Decoration: {
                const PrDrawDecoration* d = c.decoration;
                float half = d->thickness * 2 + prF32Max(d->amplitude, 0);
                local = PrRect{d->origin.x, d->origin.y - half, d->origin.x + d->length, d->origin.y + half};
                break;
            }
            case PrDrawCommandKind_NineSlice:
                local = c.nineSlice.destination;
                break;
            case PrDrawCommandKind_Embed:
                // Pixels that the embedded list writes land in the target's space, wherever this list plays.
                if (prDrawListWritesPixels(c.embed.list)) {
                    local = kUnbounded;
                } else {
                    PrRect b = prDrawListBounds(c.embed.list);
                    float x = float(c.embed.offset.x);
                    float y = float(c.embed.offset.y);
                    local = PrRect{b.left + x, b.top + y, b.right + x, b.bottom + y};
                }
                break;
            case PrDrawCommandKind_Glyphs:
                local = glyphRunBounds(c.glyphs);
                if (c.stroke != nullptr) {
                    float reach = 0;
                    strokeReach(c.stroke, &reach, &device);
                    local = outset(local, reach);
                }
                break;
            case PrDrawCommandKind_PushTransformAffine2:
                transforms.push(prDAffine2Concat(c.affine, transforms.last()));
                painting = false;
                break;
            case PrDrawCommandKind_PopTransform:
                transforms.pop();
                painting = false;
                break;
            case PrDrawCommandKind_PushClipRect:
                clips.push(prRectIntersect(clip, mapBounds(t, c.rect)));
                painting = false;
                break;
            case PrDrawCommandKind_PushClipCornerRect:
                clips.push(prRectIntersect(clip, mapBounds(t, c.cornerRect->rect)));
                painting = false;
                break;
            case PrDrawCommandKind_PushClipTargetRect:
                clips.push(prRectIntersect(clip, c.rect));
                painting = false;
                break;
            case PrDrawCommandKind_PushClipPath:
                clips.push(prRectIntersect(clip, mapBounds(t, prPathTightBounds(c.path))));
                painting = false;
                break;
            case PrDrawCommandKind_PushGroup: {
                const PrDrawGroup* g = c.group;
                if (g->projection != nullptr) {
                    // The contents are in the plane, and cover at most the plane's bounds, projected.
                    PrRect landed = prDrawProjectionBounds(g->projection, g->bounds);
                    if (!prRectIsEmpty(landed))
                        bounds = prRectUnion(bounds, prRectIntersect(clip, mapBounds(t, landed)));
                    i = prDrawCommandGroupEnd(commands, count, i);
                    painting = false;
                    break;
                }
                PrRect groupClip = clip;
                if (!prRectIsEmpty(g->bounds))
                    groupClip = prRectIntersect(clip, mapBounds(t, g->bounds));
                clips.push(groupClip);
                // These effects can cover the group's whole bounds, whatever its contents.
                if (g->filter != nullptr || g->backdrop != nullptr || g->layer != nullptr ||
                        prDrawCompositeIsUnbounded(g->composite))
                    bounds = prRectUnion(bounds, groupClip);
                painting = false;
                break;
            }
            case PrDrawCommandKind_PopClip:
            case PrDrawCommandKind_PopGroup:
                clips.pop();
                painting = false;
                break;
            case PrDrawCommandKind_PushMark:
            case PrDrawCommandKind_PopMark:
                painting = false;
                break;
            default: {
                // Shapes.
                switch (c.kind) {
                    case PrDrawCommandKind_Rect:
                        local = c.rect;
                        break;
                    case PrDrawCommandKind_CornerRect:
                        local = c.cornerRect->rect;
                        break;
                    case PrDrawCommandKind_Ellipse: {
                        float radius = prF32Max(c.ellipse.radii.x, c.ellipse.radii.y);
                        local = PrRect{
                            c.ellipse.center.x - radius, c.ellipse.center.y - radius,
                            c.ellipse.center.x + radius, c.ellipse.center.y + radius,
                        };
                        break;
                    }
                    case PrDrawCommandKind_Sector: {
                        float radius = c.sector.outerRadius;
                        local = PrRect{
                            c.sector.center.x - radius, c.sector.center.y - radius,
                            c.sector.center.x + radius, c.sector.center.y + radius,
                        };
                        break;
                    }
                    case PrDrawCommandKind_Capsule: {
                        float radius = c.capsule.radius;
                        PrVec2 a = c.capsule.start;
                        PrVec2 b = c.capsule.end;
                        local = PrRect{
                            prF32Min(a.x, b.x) - radius, prF32Min(a.y, b.y) - radius,
                            prF32Max(a.x, b.x) + radius, prF32Max(a.y, b.y) + radius,
                        };
                        break;
                    }
                    case PrDrawCommandKind_Line:
                        local = PrRect{
                            prF32Min(c.line.start.x, c.line.end.x),
                            prF32Min(c.line.start.y, c.line.end.y),
                            prF32Max(c.line.start.x, c.line.end.x),
                            prF32Max(c.line.start.y, c.line.end.y),
                        };
                        break;
                    case PrDrawCommandKind_Polyline: {
                        const PrVec2* p = c.polyline->points;
                        local = PrRect{p[0].x, p[0].y, p[0].x, p[0].y};
                        for (uint32_t k = 1; k < c.polyline->pointCount; ++k) {
                            local.left = prF32Min(local.left, p[k].x);
                            local.top = prF32Min(local.top, p[k].y);
                            local.right = prF32Max(local.right, p[k].x);
                            local.bottom = prF32Max(local.bottom, p[k].y);
                        }
                        break;
                    }
                    case PrDrawCommandKind_Path:
                        local = prPathTightBounds(c.path);
                        break;
                    default:
                        break;
                }
                if (c.stroke != nullptr) {
                    float reach = 0;
                    strokeReach(c.stroke, &reach, &device);
                    local = outset(local, reach);
                }
                break;
            }
        }
        if (!painting)
            continue;
        // An operator that changes what lies outside the shape covers the whole clip.
        if (c.kind != PrDrawCommandKind_Embed && prDrawCompositeIsUnbounded(c.composite))
            local = kUnbounded;
        PrRect mapped = mapBounds(t, local);
        if (device > 0 && !unbounded(mapped))
            mapped = outset(mapped, device);
        // A shape with no area still covers the area that its outline or antialiasing reaches.
        if (!unbounded(mapped))
            mapped = outset(mapped, 1);
        bounds = prRectUnion(bounds, prRectIntersect(mapped, clip));
    }
    transforms.reset();
    clips.reset();
    return bounds;
}

PrRect recordingBounds(const DrawRecording& r) {
    return commandsBounds(r.commands.data, r.commands.count);
}

// Returns true if a filter reads what lies beneath the group.
bool readsBackdrop(const PrDrawFilter* PR_NULLABLE filter) {
    if (filter == nullptr)
        return false;
    const PrDrawFilterNode* nodes = prDrawFilterNodes(filter);
    for (uint32_t i = 0; i < prDrawFilterNodeCount(filter); ++i) {
        const PrDrawFilterNode& n = nodes[i];
        if (n.inputs[0] == PrDrawFilterInput_Backdrop || n.inputs[1] == PrDrawFilterInput_Backdrop)
            return true;
        for (uint32_t k = 0; n.kind == PrDrawFilterKind_Merge && k < n.merge.inputCount; ++k) {
            if (n.merge.inputs[k] == PrDrawFilterInput_Backdrop)
                return true;
        }
    }
    return false;
}

// Returns true if any group among the commands, or in the lists that they play, reads what lies beneath it.
bool commandsReadBackdrop(const PrDrawCommand* commands, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = commands[i];
        if (c.kind == PrDrawCommandKind_PushGroup && (c.group->backdrop != nullptr || readsBackdrop(c.group->filter)))
            return true;
        if (c.kind == PrDrawCommandKind_Embed && prDrawListReadsBackdrop(c.embed.list))
            return true;
    }
    return false;
}

// Returns true if any of the commands, or any command in the lists that they play, writes pixels.
bool commandsWritePixels(const PrDrawCommand* commands, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = commands[i];
        // Pixels written inside a projected group land in its layer.
        if (c.kind == PrDrawCommandKind_PushGroup && c.group->projection != nullptr) {
            i = prDrawCommandGroupEnd(commands, count, i);
            continue;
        }
        if (c.kind == PrDrawCommandKind_WritePixels ||
                (c.kind == PrDrawCommandKind_Embed && prDrawListWritesPixels(c.embed.list)))
            return true;
    }
    return false;
}

// Records a push of a group that composites in sRGB.
void recordGroup(DrawList* list, const PrDrawGroup* group) {
    PrDrawCommand* c = push(list, PrDrawCommandKind_PushGroup, PrDrawCommandKind_PushGroup, PrDrawEdges_Antialiased);
    DrawRecording& r = list->recording;
    c->composite = group->composite;
    c->blendMode = group->blendMode;
    PrDrawGroup* copy = sideCopy(r, group, 1);
    if (copy->projection != nullptr) {
        PR_ASSERT(!prRectIsEmpty(copy->bounds));
        copy->projection = sideCopy(r, copy->projection, 1);
    }
    if (copy->layer != nullptr) {
        PR_ASSERT(!prRectIsEmpty(copy->bounds) && copy->layer->drawn != nullptr);
        copy->layer = sideCopy(r, copy->layer, 1);
    }
    if (copy->filter != nullptr)
        holdFilter(r, copy->filter);
    if (copy->backdrop != nullptr)
        holdFilter(r, copy->backdrop);
    if (copy->mask != nullptr) {
        PrDrawMask* mask = sideCopy(r, copy->mask, 1);
        withoutSelf(list, mask->brush);
        holdBrush(r, mask->brush);
        copy->mask = mask;
    }
    copy->space = PrColorSpace_Srgb;
    c->group = copy;
    list->groups.push(r.commands.count - 1);
}

// Closes the innermost open group. A group without bounds whose contents alone decide what it covers takes their
// bounds, so that a renderer sizes its layer to them and skips the group where it does not show. A filter or a backdrop
// may draw outside the contents, and so may an operator that changes what lies outside them. A group that draws
// nothing, because it is transparent or its contents cover nothing, is removed with its contents. A group in a knockout
// group, a group that draws into the host's texture, and contents that write pixels are kept.
void popGroup(DrawList* list) {
    if (list->open.count == 0 || list->open.last() != PrDrawCommandKind_PushGroup) {
        pop(list, PrDrawCommandKind_PushGroup, PrDrawCommandKind_PopGroup);
        return;
    }
    uint32_t push = list->groups.last();
    list->groups.pop();
    pop(list, PrDrawCommandKind_PushGroup, PrDrawCommandKind_PopGroup);
    RawArray<PrDrawCommand>& commands = list->recording.commands;
    const PrDrawCommand* contents = commands.data + push + 1;
    uint32_t count = commands.count - push - 2;
    // The list owns the side copy of the group, which nothing has read yet.
    PrDrawGroup* group = const_cast<PrDrawGroup*>(commands.data[push].group);
    bool bounded = !prDrawCompositeIsUnbounded(group->composite);
    bool removable = bounded && group->layer == nullptr &&
        !(list->groups.count != 0 && commands.data[list->groups.last()].group->knockout) &&
        !commandsWritePixels(contents, count);
    if (removable && !(group->opacity > 0)) {
        commands.resize(push);
        return;
    }
    if (!prRectIsEmpty(group->bounds) || group->filter != nullptr || group->backdrop != nullptr || !bounded)
        return;
    PrRect covered = commandsBounds(contents, count);
    if (prRectIsEmpty(covered)) {
        if (removable)
            commands.resize(push);
    } else if (!unbounded(covered)) {
        group->bounds = covered;
    }
}

// Records a group that composites in sRGB-linear as two groups that composite in sRGB. The outer group reads what lies
// beneath, blends its contents onto it in sRGB-linear by a filter, and copies the result over it. The inner group holds
// the group's other effects, and is left out if it has none.
void pushLinearGroup(DrawList* list, const PrDrawGroup* group) {
    PR_ASSERT(group->backdrop == nullptr);
    PrDrawFilterNode node{
        .space = PrColorSpace_SrgbLinear,
        .inputs = {PrDrawFilterInput_Source, PrDrawFilterInput_Backdrop},
    };
    if (group->composite == PrDrawComposite_SourceOver) {
        node.kind = PrDrawFilterKind_Blend;
        node.blend = group->blendMode;
    } else {
        node.kind = PrDrawFilterKind_Composite;
        node.composite = group->composite;
    }
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    prDrawFilterBuilderAdd(builder, &node);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawGroup outer = prDrawGroupDefault();
    outer.bounds = group->bounds;
    outer.composite = PrDrawComposite_Copy;
    outer.filter = filter;
    recordGroup(list, &outer);
    prDrawFilterRelease(filter);
    PrDrawGroup inner = *group;
    inner.space = PrColorSpace_Srgb;
    inner.composite = PrDrawComposite_SourceOver;
    inner.blendMode = PrDrawBlendMode_Normal;
    bool nested = prDrawGroupHasEffect(&inner);
    if (nested)
        recordGroup(list, &inner);
    list->lowered.push(DrawLoweredGroup{.depth = list->open.count, .nested = nested});
}

void pushGroup(DrawList* list, const PrDrawGroup* group) {
    if (group->space != PrColorSpace_Srgb && group->projection == nullptr)
        pushLinearGroup(list, group);
    else
        recordGroup(list, group);
}

} // namespace

void drawListFreeze(DrawList* list) {
    if (!list->frozen) {
        if (list->id == 0)
            list->id = drawNextId();
        list->bounds = recordingBounds(list->recording);
        list->writesPixels = commandsWritePixels(list->recording.commands.data, list->recording.commands.count);
        list->readsBackdrop = commandsReadBackdrop(list->recording.commands.data, list->recording.commands.count);
        list->frozen = true;
    }
}

const RawArray<PrDrawCommand>& drawListCommands(const DrawList* list) {
    return list->recording.commands;
}

void drawListRetain(const DrawList* list) {
    list->references.fetchAdd(1);
}

void drawListRelease(const DrawList* PR_NULLABLE list) {
    if (list == nullptr || list->references.fetchSub(1) != 1)
        return;
    // The last reference owns the list, which no holder reads any longer.
    DrawList* owned = const_cast<DrawList*>(list);
    resetRecording(owned->recording);
    owned->open.reset();
    owned->lowered.reset();
    owned->groups.reset();
    prMemoryFree(owned);
}

} // namespace pr_impl

using namespace pr_impl;

PrDrawList* prDrawListCreate(void) {
    PrDrawList* list = allocZeroed<PrDrawList>();
    list->references.storeRelaxed(1);
    return list;
}

void prDrawListRetain(PrDrawList* list) {
    drawListRetain(list);
}

void prDrawListRelease(PrDrawList* PR_NULLABLE list) {
    drawListRelease(list);
}

void prDrawListHold(PrDrawList* list, void (*release)(void* PR_NULLABLE context), void* PR_NULLABLE context) {
    PR_ASSERT(!list->frozen);
    list->recording.held.push(DrawHeld{release, context});
}

void prDrawListClear(PrDrawList* list) {
    changed(list);
    clearRecording(list->recording);
    list->open.clear();
    list->lowered.clear();
    list->groups.clear();
    list->lastBrush = nullptr;
    list->lastStroke = nullptr;
}

void prDrawListClearRect(PrDrawList* list, PrRect rect, PrColor16 color) {
    if (prRectIsEmpty(rect))
        return;
    prDrawListPushClipRect(list, rect, PrDrawEdges_Antialiased);
    add(list, PrDrawCommandKind_Clear, PrDrawEdges_Antialiased)->color = color;
    prDrawListPopClip(list);
}

void prDrawListRect(PrDrawList* list, PrRect rect, PrDrawPaint paint) {
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Rect, !prRectIsEmpty(rect), paint))
        c->rect = rect;
}

void prDrawListCornerRect(PrDrawList* list, PrCornerRect shape, PrDrawPaint paint) {
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_CornerRect, !prRectIsEmpty(shape.rect), paint))
        c->cornerRect = sideCopy(list->recording, &shape, 1);
}

void prDrawListEllipse(PrDrawList* list, PrVec2 center, PrVec2 radii, float rotation, PrDrawPaint paint) {
    bool area = radii.x > 0 && radii.y > 0;
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Ellipse, area, paint)) {
        c->ellipse.center = center;
        c->ellipse.radii = radii;
        c->ellipse.rotation = rotation;
    }
}

void prDrawListSector(PrDrawList* list, PrVec2 center, float innerRadius, float outerRadius, float startAngle,
        float sweep, PrDrawPaint paint) {
    bool area = outerRadius > 0 && sweep != 0 && innerRadius < outerRadius;
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Sector, area, paint)) {
        c->sector.center = center;
        c->sector.innerRadius = innerRadius;
        c->sector.outerRadius = outerRadius;
        c->sector.startAngle = startAngle;
        c->sector.sweep = sweep;
    }
}

void prDrawListCapsule(PrDrawList* list, PrVec2 start, PrVec2 end, float radius, PrDrawPaint paint) {
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Capsule, radius > 0, paint)) {
        c->capsule.start = start;
        c->capsule.end = end;
        c->capsule.radius = radius;
    }
}

void prDrawListLine(PrDrawList* list, PrVec2 start, PrVec2 end, PrDrawPaint paint) {
    PR_ASSERT(paint.stroke != nullptr);
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Line, false, paint)) {
        c->line.start = start;
        c->line.end = end;
    }
}

void prDrawListPolyline(PrDrawList* list, const PrPathPolyline* polyline, PrPathFillRule rule, PrDrawPaint paint) {
    if (polyline->pointCount == 0 || polyline->contourCount == 0)
        return;
    bool area = false;
    for (uint32_t i = 0; i < polyline->contourCount && !area; ++i)
        area = prPathPolylineContourEnd(polyline, i) - polyline->contours[i] >= 3;
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Polyline, area, paint)) {
        DrawRecording& r = list->recording;
        PrPathPolyline* copy = sideCopy(r, polyline, 1);
        copy->points = sideCopy(r, polyline->points, polyline->pointCount);
        copy->contours = sideCopy(r, polyline->contours, polyline->contourCount);
        copy->closed = sideCopy(r, polyline->closed, uint32_t(bitsetWords(polyline->contourCount)));
        copy->pointCapacity = copy->pointCount;
        copy->contourCapacity = copy->contourCount;
        c->rule = rule;
        c->polyline = copy;
    }
}

void prDrawListPolylineContour(PrDrawList* list, const PrVec2* points, uint32_t pointCount, PrDrawContour contour,
        PrPathFillRule rule, PrDrawPaint paint) {
    uint32_t start = 0;
    uint64_t closedBits = contour == PrDrawContour_Closed ? 1 : 0;
    PrPathPolyline polyline{const_cast<PrVec2*>(points), &start, &closedBits, pointCount, pointCount, 1, 1};
    prDrawListPolyline(list, &polyline, rule, paint);
}

void prDrawListPath(PrDrawList* list, const PrPath* path, PrPathFillRule rule, PrDrawPaint paint) {
    if (prPathVerbCount(path) == 0)
        return;
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Path, true, paint)) {
        c->rule = rule;
        c->path = holdPath(list->recording, path);
    }
}

void prDrawListBorder(PrDrawList* list, const PrDrawBorder* border, PrDrawPaint paint) {
    PrDrawBrush brushes[4];
    bool paints = false;
    for (uint32_t i = 0; i < 4; ++i) {
        const PrDrawBorderSide& side = border->sides[i];
        if (side.brush != nullptr) {
            brushes[i] = *side.brush;
            withoutSelf(list, brushes[i]);
        }
        paints = paints || (side.width > 0 && prDrawBorderStylePaints(side.style) &&
            (side.brush != nullptr ? brushPaints(&brushes[i]) && brushes[i].opacity > 0 : prFloat16ToFloat(side.color.a) > 0));
    }
    if (prRectIsEmpty(border->shape.rect) || !paints)
        return;
    DrawRecording& r = list->recording;
    PrDrawCommand* c = add(list, PrDrawCommandKind_Border, paint.edges);
    c->composite = paint.composite;
    c->blendMode = paint.blendMode;
    PrDrawBorder* copy = sideCopy(r, border, 1);
    for (uint32_t i = 0; i < 4; ++i) {
        if (copy->sides[i].brush != nullptr)
            copy->sides[i].brush = copyBrush(r, &brushes[i]);
    }
    c->border = copy;
}

void prDrawListDropShadow(PrDrawList* list, const PrDrawShadow* shadow, PrDrawPaint paint) {
    PR_ASSERT(paint.stroke == nullptr);
    if (prRectIsEmpty(shadow->shape.rect))
        return;
    if (PrDrawCommand* c = addPainting(list, PrDrawCommandKind_DropShadow, paint))
        c->shadow = sideCopy(list->recording, shadow, 1);
}

void prDrawListInnerShadow(PrDrawList* list, const PrDrawShadow* shadow, PrDrawPaint paint) {
    PR_ASSERT(paint.stroke == nullptr);
    if (prRectIsEmpty(shadow->shape.rect))
        return;
    if (PrDrawCommand* c = addPainting(list, PrDrawCommandKind_InnerShadow, paint))
        c->shadow = sideCopy(list->recording, shadow, 1);
}

void prDrawListDecoration(PrDrawList* list, const PrDrawDecoration* decoration, PrDrawPaint paint) {
    PR_ASSERT(paint.stroke == nullptr);
    if (!(decoration->length > 0 && decoration->thickness > 0))
        return;
    if (PrDrawCommand* c = addPainting(list, PrDrawCommandKind_Decoration, paint))
        c->decoration = sideCopy(list->recording, decoration, 1);
}

void prDrawListNineSlice(PrDrawList* list, PrDrawSource source, const PrDrawNineSlice* slices, PrRect destination,
        PrDrawSampling sampling, PrDrawPaint paint) {
    PR_ASSERT(paint.brush == nullptr && paint.stroke == nullptr);
    if (!prDrawSourceIsValid(source) || prRectIsEmpty(destination) ||
            (source.kind == PrDrawSourceKind_List && source.list == list))
        return;
    if (PrDrawCommand* c = addPainting(list, PrDrawCommandKind_NineSlice, paint)) {
        holdSource(list->recording, source);
        c->sampling = sampling;
        c->nineSlice.source = source;
        c->nineSlice.destination = destination;
        c->nineSlice.slices = sideCopy(list->recording, slices, 1);
    }
}

void prDrawListWritePixels(PrDrawList* list, const PrDrawImage* image, PrIVec2 origin) {
    holdImage(list->recording, image);
    PrDrawCommand* c = add(list, PrDrawCommandKind_WritePixels, PrDrawEdges_Antialiased);
    c->writePixels.image = image;
    c->writePixels.origin = origin;
}

void prDrawListGlyphs(PrDrawList* list, const PrFontGlyphRun* run, PrVec2 origin, PrDrawPaint paint) {
    if (run->glyphCount == 0 || !(run->size > 0) || !prF32IsFinite(run->size))
        return;
    if (PrDrawCommand* c = addShape(list, PrDrawCommandKind_Glyphs, true, paint)) {
        DrawRecording& r = list->recording;
        PrFontGlyphRun* copy = sideCopy(r, run, 1);
        PrFontGlyph* glyphs = sideCopy(r, run->glyphs, run->glyphCount);
        if (origin.x != 0 || origin.y != 0) {
            for (uint32_t i = 0; i < run->glyphCount; ++i)
                glyphs[i].position = glyphs[i].position + origin;
        }
        copy->glyphs = glyphs;
        if (run->transforms != nullptr)
            copy->transforms = sideCopy(r, run->transforms, run->glyphCount);
        if (run->palette.colors != nullptr && run->palette.colorCount != 0)
            copy->palette.colors = sideCopy(r, run->palette.colors, run->palette.colorCount);
        else
            copy->palette.colorCount = 0;
        if (run->text.data != nullptr && run->text.size != 0 && run->text.size < UINT32_MAX) {
            copy->text.data = sideCopy(r, run->text.data, uint32_t(run->text.size));
        } else {
            copy->text = PrStringView{};
            copy->clusters = nullptr;
        }
        if (copy->clusters != nullptr) {
            for (uint32_t i = 0; i < run->glyphCount; ++i)
                PR_ASSERT(run->clusters[i] < run->text.size);
            copy->clusters = sideCopy(r, run->clusters, run->glyphCount);
        }
        holdFont(r, copy->font);
        c->glyphs = copy;
        if (prFontIsAliased(copy->font))
            c->antialias = false;
    }
}

void prDrawListEmbed(PrDrawList* list, PrDrawList* embedded, PrDVec2 offset) {
    if (embedded == list || prDrawListCommandCount(embedded) == 0 || prDrawListDepth(embedded) != 0)
        return;
    drawListFreeze(embedded);
    holdList(list->recording, embedded);
    PrDrawCommand* c = add(list, PrDrawCommandKind_Embed, PrDrawEdges_Antialiased);
    c->embed.list = embedded;
    c->embed.offset = offset;
}

void prDrawListFreeze(PrDrawList* list) {
    drawListFreeze(list);
}

void prDrawListPushTransformAffine2(PrDrawList* list, PrAffine2 transform) {
    prDrawListPushTransformDAffine2(list, prDAffine2FromAffine2(transform));
}

void prDrawListPushTransformDAffine2(PrDrawList* list, PrDAffine2 transform) {
    prDrawListPushTransformSnapped(list, transform, 0);
}

void prDrawListPopTransform(PrDrawList* list) {
    pop(list, PrDrawCommandKind_PushTransformAffine2, PrDrawCommandKind_PopTransform);
}

void prDrawListPushTransformSnapped(PrDrawList* list, PrDAffine2 transform, PrDrawSnapFlags flags) {
    PrDrawCommand* c = push(list, PrDrawCommandKind_PushTransformAffine2, PrDrawCommandKind_PushTransformAffine2,
        PrDrawEdges_Antialiased);
    c->affine = transform;
    c->snap = flags;
}

void prDrawListPushTransformSnappedRect(PrDrawList* list, PrDAffine2 transform, PrVec2 extent) {
    prDrawListPushTransformSnapped(list, transform, PrDrawSnapFlag_Rect);
    list->recording.commands.last().snapExtent = extent;
}

void prDrawListPushClipRect(PrDrawList* list, PrRect rect, PrDrawEdges edges) {
    push(list, PrDrawCommandKind_PushClipRect, PrDrawCommandKind_PushClipRect, edges)->rect = rect;
}

void prDrawListPushClipTargetRect(PrDrawList* list, PrRect targetRect, PrDrawEdges edges) {
    push(list, PrDrawCommandKind_PushClipTargetRect, PrDrawCommandKind_PushClipRect, edges)->rect = targetRect;
}

void prDrawListPushClipCornerRect(PrDrawList* list, PrCornerRect shape, PrDrawEdges edges) {
    PrDrawCommand* c = push(list, PrDrawCommandKind_PushClipCornerRect, PrDrawCommandKind_PushClipRect, edges);
    c->cornerRect = sideCopy(list->recording, &shape, 1);
}

void prDrawListPushClipPath(PrDrawList* list, const PrPath* path, PrPathFillRule rule, PrDrawEdges edges) {
    PrDrawCommand* c = push(list, PrDrawCommandKind_PushClipPath, PrDrawCommandKind_PushClipRect, edges);
    c->rule = rule;
    c->path = holdPath(list->recording, path);
}

void prDrawListPopClip(PrDrawList* list) {
    pop(list, PrDrawCommandKind_PushClipRect, PrDrawCommandKind_PopClip);
}

void prDrawListPushGroup(PrDrawList* list, const PrDrawGroup* group) {
    pushGroup(list, group);
}

void prDrawListPopGroup(PrDrawList* list) {
    // A group that composites in sRGB-linear may have been recorded as two groups, which this pop closes together.
    bool lowered = list->lowered.count != 0 && list->lowered.last().depth == list->open.count;
    bool nested = lowered && list->lowered.last().nested;
    if (lowered)
        list->lowered.pop();
    popGroup(list);
    if (nested)
        popGroup(list);
}

void prDrawListPushMark(PrDrawList* list, uint64_t key) {
    PR_ASSERT(key != 0);
    push(list, PrDrawCommandKind_PushMark, PrDrawCommandKind_PushMark, PrDrawEdges_Antialiased)->key = key;
}

void prDrawListPopMark(PrDrawList* list) {
    pop(list, PrDrawCommandKind_PushMark, PrDrawCommandKind_PopMark);
}

uint32_t prDrawListDepth(const PrDrawList* list) {
    return list->open.count;
}

PrRect prDrawListBounds(const PrDrawList* list) {
    return list->frozen ? list->bounds : recordingBounds(list->recording);
}

bool prDrawListMisses(const PrDrawList* list, PrAffine2 transform, PrRect clip) {
    if (prDrawListWritesPixels(list))
        return false;
    PrRect bounds = prDrawListBounds(list);
    if (prRectIsEmpty(bounds) || prRectIsEmpty(clip))
        return true;
    if (unbounded(bounds) || !prF32IsFinite(bounds.left) || !prF32IsFinite(bounds.top) ||
            !prF32IsFinite(bounds.right) || !prF32IsFinite(bounds.bottom))
        return false;
    PrRect mapped = prAffine2MapRect(transform, bounds);
    if (!prF32IsFinite(mapped.left) || !prF32IsFinite(mapped.top) || !prF32IsFinite(mapped.right) ||
            !prF32IsFinite(mapped.bottom))
        return false;
    return mapped.right <= clip.left || mapped.left >= clip.right || mapped.bottom <= clip.top ||
        mapped.top >= clip.bottom;
}

PrRect prDrawCommandsBounds(const PrDrawCommand* PR_NULLABLE commands, uint32_t count) {
    return count != 0 ? commandsBounds(commands, count) : PrRect{0, 0, 0, 0};
}

bool prDrawListReadsBackdrop(const PrDrawList* list) {
    if (list->frozen)
        return list->readsBackdrop;
    return commandsReadBackdrop(list->recording.commands.data, list->recording.commands.count);
}

bool prDrawListWritesPixels(const PrDrawList* list) {
    if (list->frozen)
        return list->writesPixels;
    return commandsWritePixels(list->recording.commands.data, list->recording.commands.count);
}

uint64_t prDrawListId(const PrDrawList* list) {
    if (list->id == 0)
        list->id = drawNextId();
    return list->id;
}
