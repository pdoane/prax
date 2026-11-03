// Draw commands: the contents of a recorded list, in the form that a backend reads.

#include "pr/draw_commands.h"

#include "pr/float.h"
#include "pr/vec.h"

#include "draw/src/draw_list.h"

namespace pr_impl {

namespace {

void appendText(PrRawString* out, const char* text) {
    size_t size = 0;
    while (text[size] != 0)
        ++size;
    prRawStringAppend(out, PrStringView{text, size});
}

void appendNumbers(PrRawString* out, const float* values, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        char text[kPrF64TextMaxBytes + 1];
        text[0] = ' ';
        size_t size = prF64Format(double(values[i]), text + 1);
        prRawStringAppend(out, PrStringView{text, size + 1});
    }
}

void appendNumber(PrRawString* out, float value) {
    appendNumbers(out, &value, 1);
}

void appendPoints(PrRawString* out, const PrVec2* points, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        float values[2] = {points[i].x, points[i].y};
        appendNumbers(out, values, 2);
    }
}

void appendRect(PrRawString* out, PrRect rect) {
    float values[4] = {rect.left, rect.top, rect.right, rect.bottom};
    appendNumbers(out, values, 4);
}

void appendColor(PrRawString* out, const char* label, PrColor c) {
    float values[4] = {c.r, c.g, c.b, c.a};
    appendText(out, label);
    appendNumbers(out, values, 4);
}

void appendCorners(PrRawString* out, const PrCorner* corners) {
    appendText(out, " corners");
    for (uint32_t i = 0; i < 4; ++i) {
        float values[3] = {corners[i].radii.x, corners[i].radii.y, corners[i].shape};
        appendNumbers(out, values, 3);
    }
}

void appendPath(PrRawString* out, const PrPath* path) {
    constexpr const char* kVerbName[] = {" M", " L", " Q", " C", " Z"};
    constexpr uint32_t kVerbPoints[] = {1, 1, 2, 3, 0};
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t next = 0;
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        appendText(out, kVerbName[verbs[i]]);
        appendPoints(out, points + next, kVerbPoints[verbs[i]]);
        next += kVerbPoints[verbs[i]];
    }
}

void appendRule(PrRawString* out, PrPathFillRule rule) {
    appendText(out, rule == PrPathFillRule_EvenOdd ? " evenodd" : " nonzero");
}

void appendBrush(PrRawString* out, const PrDrawBrush* brush) {
    constexpr const char* kKind[] = {" linear", " radial", " conic", " diamond", " image"};
    appendText(out, " brush");
    appendText(out, kKind[brush->kind]);
    for (uint32_t i = 0; i < brush->stopCount; ++i) {
        appendText(out, " stop");
        appendNumber(out, brush->stops[i].offset);
        appendColor(out, "", brush->stops[i].color);
    }
}

void appendPaintState(PrRawString* out, const PrDrawCommand& command) {
    if (command.stroke != nullptr) {
        constexpr const char* kJoin[] = {" miter", " miter-clip", " round", " bevel", " arcs"};
        constexpr const char* kCap[] = {" butt", " round", " square", " triangle"};
        constexpr const char* kAlign[] = {"", " inside", " outside"};
        constexpr const char* kMapping[] = {"", " along", " across"};
        const PrPathStroke& shape = command.stroke->shape;
        appendText(out, " stroke");
        float values[3] = {shape.width, shape.miterLimit, shape.dashOffset};
        appendNumbers(out, values, 3);
        appendText(out, kJoin[shape.join]);
        appendText(out, kCap[shape.startCap]);
        appendText(out, kCap[shape.endCap]);
        appendText(out, kCap[shape.dashCap]);
        if (shape.zeroLengthDirection.x != 1 || shape.zeroLengthDirection.y != 0) {
            appendText(out, " facing");
            float direction[2] = {shape.zeroLengthDirection.x, shape.zeroLengthDirection.y};
            appendNumbers(out, direction, 2);
        }
        if (shape.dashCount != 0) {
            appendText(out, " dashes");
            appendNumbers(out, shape.dashes, shape.dashCount);
        }
        if (shape.profileCount != 0) {
            appendText(out, " profile");
            for (uint32_t i = 0; i < shape.profileCount; ++i) {
                float point[3] = {shape.profile[i].position, shape.profile[i].left, shape.profile[i].right};
                appendNumbers(out, point, 3);
            }
        }
        if (command.stroke->nonScaling)
            appendText(out, " non-scaling");
        appendText(out, kAlign[command.stroke->align]);
        appendText(out, kMapping[command.stroke->mapping]);
    }
    if (command.brush != nullptr)
        appendBrush(out, command.brush);
    appendColor(out, " color", command.color);
    if (command.composite != PrDrawComposite_SourceOver) {
        appendText(out, " composite");
        appendNumber(out, float(command.composite));
    }
    if (command.blendMode != PrDrawBlendMode_Normal) {
        appendText(out, " blend");
        appendNumber(out, float(command.blendMode));
    }
    if (!command.antialias)
        appendText(out, " aliased");
    if (command.snap != 0)
        appendText(out, " snapped");
}

void appendShadow(PrRawString* out, const PrDrawShadow* s) {
    appendRect(out, s->shape.rect);
    appendCorners(out, s->shape.corners);
    appendText(out, " offset");
    appendPoints(out, &s->offset, 1);
    appendText(out, " blur");
    appendNumber(out, s->sigma);
    appendText(out, " spread");
    appendNumber(out, s->spread);
    if (s->nonScaling)
        appendText(out, " non-scaling");
}

void dumpCommand(PrRawString* out, const PrDrawCommand& command) {
    switch (command.kind) {
        case PrDrawCommandKind_Clear:
            appendColor(out, "clear", command.color);
            break;
        case PrDrawCommandKind_Rect:
            appendText(out, "rect");
            appendRect(out, command.rect);
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_CornerRect:
            appendText(out, "corner-rect");
            appendRect(out, command.cornerRect->rect);
            appendCorners(out, command.cornerRect->corners);
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_Ellipse:
            appendText(out, "ellipse");
            appendPoints(out, &command.ellipse.center, 1);
            appendPoints(out, &command.ellipse.radii, 1);
            appendNumber(out, command.ellipse.rotation);
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_Sector: {
            float values[4] = {
                command.sector.innerRadius, command.sector.outerRadius,
                command.sector.startAngle, command.sector.sweep,
            };
            appendText(out, "sector");
            appendPoints(out, &command.sector.center, 1);
            appendNumbers(out, values, 4);
            appendPaintState(out, command);
            break;
        }
        case PrDrawCommandKind_Capsule:
            appendText(out, "capsule");
            appendPoints(out, &command.capsule.start, 1);
            appendPoints(out, &command.capsule.end, 1);
            appendNumber(out, command.capsule.radius);
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_Line:
            appendText(out, "line");
            appendPoints(out, &command.line.start, 1);
            appendPoints(out, &command.line.end, 1);
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_Polyline: {
            const PrPathPolyline& polyline = *command.polyline;
            appendText(out, "polyline");
            for (uint32_t i = 0; i < polyline.contourCount; ++i) {
                appendText(out, prPathPolylineContourIsClosed(&polyline, i) ? " closed" : " open");
                uint32_t start = polyline.contours[i];
                appendPoints(out, polyline.points + start, prPathPolylineContourEnd(&polyline, i) - start);
            }
            if (command.stroke == nullptr)
                appendRule(out, command.rule);
            appendPaintState(out, command);
            break;
        }
        case PrDrawCommandKind_Path:
            appendText(out, "path");
            appendPath(out, command.path);
            if (command.stroke == nullptr)
                appendRule(out, command.rule);
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_Border: {
            constexpr const char* kStyle[] = {
                " none",  " solid", " double", " dotted", " dashed",
                " groove", " ridge", " inset",  " outset",
            };
            appendText(out, "border");
            appendRect(out, command.border->shape.rect);
            appendCorners(out, command.border->shape.corners);
            for (const PrDrawBorderSide& side : command.border->sides) {
                appendText(out, " side");
                appendNumber(out, side.width);
                appendText(out, kStyle[side.style]);
                appendColor(out, "", side.color);
                if (side.brush != nullptr)
                    appendBrush(out, side.brush);
            }
            if (!command.antialias)
                appendText(out, " aliased");
            if (command.snap != 0)
                appendText(out, " snapped");
            break;
        }
        case PrDrawCommandKind_DropShadow:
        case PrDrawCommandKind_InnerShadow:
            appendText(out, command.kind == PrDrawCommandKind_DropShadow ? "drop-shadow" : "inner-shadow");
            appendShadow(out, command.shadow);
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_Decoration: {
            constexpr const char* kStyle[] = {" solid", " double", " dotted", " dashed", " wavy"};
            const PrDrawDecoration* d = command.decoration;
            float values[5] = {d->length, d->thickness, d->wavelength, d->amplitude, d->phase};
            appendText(out, "decoration");
            appendPoints(out, &d->origin, 1);
            appendNumbers(out, values, 5);
            appendText(out, kStyle[d->style]);
            appendPaintState(out, command);
            break;
        }
        case PrDrawCommandKind_NineSlice:
            appendText(out, "nine-slice");
            if (command.nineSlice.source.kind == PrDrawSourceKind_Image) {
                appendNumber(out, float(prDrawImageExtent(command.nineSlice.source.image).x));
                appendNumber(out, float(prDrawImageExtent(command.nineSlice.source.image).y));
            } else {
                appendText(out, command.nineSlice.source.kind == PrDrawSourceKind_List ? " list" : " texture");
            }
            appendRect(out, command.nineSlice.source.rect);
            appendNumbers(out, command.nineSlice.slices->slices, 4);
            appendNumbers(out, command.nineSlice.slices->widths, 4);
            appendRect(out, command.nineSlice.destination);
            appendText(out, " sampling");
            appendNumber(out, float(command.sampling));
            appendPaintState(out, command);
            break;
        case PrDrawCommandKind_WritePixels:
            appendText(out, "write-pixels");
            appendNumber(out, float(command.writePixels.origin.x));
            appendNumber(out, float(command.writePixels.origin.y));
            break;
        case PrDrawCommandKind_Glyphs: {
            const PrFontGlyphRun* run = command.glyphs;
            appendText(out, "glyphs size");
            appendNumber(out, run->size);
            for (uint32_t i = 0; i < run->glyphCount; ++i) {
                appendNumber(out, float(run->glyphs[i].id));
                appendPoints(out, &run->glyphs[i].position, 1);
            }
            if (run->transforms != nullptr)
                appendText(out, " transforms");
            if (run->palette.index != 0 || run->palette.colorCount != 0) {
                appendText(out, " palette");
                appendNumber(out, float(run->palette.index));
                for (uint32_t i = 0; i < run->palette.colorCount; ++i) {
                    appendNumber(out, float(run->palette.colors[i].index));
                    appendColor(out, "", run->palette.colors[i].color);
                }
            }
            appendPaintState(out, command);
            break;
        }
        case PrDrawCommandKind_Embed:
            appendText(out, "embed");
            appendNumber(out, float(prDrawListCommandCount(command.embed.list)));
            if (command.embed.offset.x != 0 || command.embed.offset.y != 0) {
                appendText(out, " at");
                appendNumber(out, float(command.embed.offset.x));
                appendNumber(out, float(command.embed.offset.y));
            }
            break;
        case PrDrawCommandKind_PushTransformAffine2: {
            PrAffine2 t = prAffine2FromDAffine2(command.affine);
            float values[6] = {t.a, t.b, t.c, t.d, t.e, t.f};
            appendText(out, "push-transform-affine");
            appendNumbers(out, values, 6);
            if ((command.snap & PrDrawSnapFlag_X) != 0)
                appendText(out, " snap-x");
            if ((command.snap & PrDrawSnapFlag_Y) != 0)
                appendText(out, " snap-y");
            if ((command.snap & PrDrawSnapFlag_Apart) != 0)
                appendText(out, " snap-apart");
            if ((command.snap & PrDrawSnapFlag_Rect) != 0) {
                appendText(out, " snap-rect");
                appendNumber(out, command.snapExtent.x);
                appendNumber(out, command.snapExtent.y);
            }
            break;
        }
        case PrDrawCommandKind_PopTransform:
            appendText(out, "pop-transform");
            break;
        case PrDrawCommandKind_PushClipRect:
        case PrDrawCommandKind_PushClipTargetRect:
            appendText(out, command.kind == PrDrawCommandKind_PushClipRect ? "push-clip-rect" :
                "push-clip-target-rect");
            appendRect(out, command.rect);
            break;
        case PrDrawCommandKind_PushClipCornerRect:
            appendText(out, "push-clip-corner-rect");
            appendRect(out, command.cornerRect->rect);
            appendCorners(out, command.cornerRect->corners);
            break;
        case PrDrawCommandKind_PushClipPath:
            appendText(out, "push-clip-path");
            appendPath(out, command.path);
            appendRule(out, command.rule);
            break;
        case PrDrawCommandKind_PopClip:
            appendText(out, "pop-clip");
            break;
        case PrDrawCommandKind_PushGroup: {
            const PrDrawGroup* g = command.group;
            appendText(out, "push-group");
            appendRect(out, g->bounds);
            appendText(out, " opacity");
            appendNumber(out, g->opacity);
            if (g->composite != PrDrawComposite_SourceOver) {
                appendText(out, " composite");
                appendNumber(out, float(g->composite));
            }
            if (g->blendMode != PrDrawBlendMode_Normal) {
                appendText(out, " blend");
                appendNumber(out, float(g->blendMode));
            }
            if (g->isolated)
                appendText(out, " isolated");
            if (g->knockout)
                appendText(out, " knockout");
            if (g->filter != nullptr)
                appendText(out, " filter");
            if (g->backdrop != nullptr)
                appendText(out, " backdrop");
            if (g->mask != nullptr) {
                appendText(out, g->mask->mode == PrDrawMaskMode_Alpha ? " mask alpha" : " mask luminance");
                appendBrush(out, &g->mask->brush);
            }
            if (g->projection != nullptr) {
                appendText(out, " projection");
                appendNumbers(out, &g->projection->m[0][0], 16);
            }
            break;
        }
        case PrDrawCommandKind_PopGroup:
            appendText(out, "pop-group");
            break;
        default:
            break;
    }
    if (command.kind >= PrDrawCommandKind_PushClipRect &&
            command.kind <= PrDrawCommandKind_PushClipPath && !command.antialias)
        appendText(out, " aliased");
    if (command.kind >= PrDrawCommandKind_PushClipRect && command.kind <= PrDrawCommandKind_PushClipPath && command.snap != 0)
        appendText(out, " snapped");
    appendText(out, "\n");
}

// Appends the list's commands at `depth`. The commands of each embedded list follow its Embed command, one level
// deeper.
void dumpList(PrRawString* out, const PrDrawList* list, uint32_t depth) {
    const RawArray<PrDrawCommand>& commands = drawListCommands(list);
    for (uint32_t i = 0; i < commands.count; ++i) {
        for (uint32_t k = 0; k < depth; ++k)
            appendText(out, "  ");
        dumpCommand(out, commands.data[i]);
        if (commands.data[i].kind == PrDrawCommandKind_Embed)
            dumpList(out, commands.data[i].embed.list, depth + 1);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prDrawListCommandCount(const PrDrawList* list) {
    return drawListCommands(list).count;
}

const PrDrawCommand* PR_NULLABLE prDrawListCommands(const PrDrawList* list) {
    return drawListCommands(list).data;
}

uint32_t prDrawCommandGroupEnd(const PrDrawCommand* commands, uint32_t count, uint32_t push) {
    uint32_t depth = 0;
    for (uint32_t i = push; i < count; ++i) {
        if (commands[i].kind == PrDrawCommandKind_PushGroup)
            ++depth;
        else if (commands[i].kind == PrDrawCommandKind_PopGroup && --depth == 0)
            return i;
    }
    return count;
}

PrRawString prDrawListDump(const PrDrawList* list) {
    PrRawString out = prRawStringEmpty();
    dumpList(&out, list, 0);
    return out;
}

