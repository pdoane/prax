// The fixture of the SVG tests: a standalone document drawn into a list, and its painting commands as a test reads
// them.

#pragma once

#include "pr/array.h"
#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/draw_commands.h"
#include "pr/draw_cpu.h"
#include "pr/draw_renderer.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/path.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"

#include "tools/harness/test.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

using namespace pr;

// Returns `body` as the content of an outermost svg element in SVG's namespace, with xlink's declared.
inline String svgDocument(PrStringView body) {
    return format("<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>{}</svg>", body);
}

// A painting command, with its geometry in the space that the document is drawn in.
struct Drawn {
    PrDrawCommandKind kind;
    PrRect bounds;                  // the geometry's bounds before stroking, mapped into the drawing's space
    PrAffine2 transform;            // the user space of the command to the drawing's space
    PrColor color;
    bool brushed;                         // painted with a brush, such as a gradient's
    const PrDrawBrush* PR_NULLABLE brush; // the brush, valid while the drawing lives
    bool antialias;
    float opacity;                  // the product of the opacities of the groups that hold the command
    uint32_t clips;                 // the number of clips that hold the command

    // The stroke, if the command strokes, with its dashes copied.
    bool stroked;
    PrPathStroke stroke;
    bool nonScaling;
    Array<float> dashes;

    PrPathFillRule rule;                      // Path
    PrVec2 radii;                             // Ellipse, and the first corner of a CornerRect
    Array<PrPathVerb> verbs;                  // Path
    const PrFontGlyphRun* PR_NULLABLE glyphs; // Glyphs, valid while the drawing lives
};

// A document drawn into a list, and the painting commands of the list in order.
struct Drawing {
    PrDomDocument* document = nullptr;
    PrDrawList* list = nullptr;
    Array<Drawn> commands;
    PrVec2 extent{};

    // Draws the document `source` in a viewport of `viewport`, with `options`, or the defaults if null.
    explicit Drawing(PrStringView source, PrVec2 viewport = PrVec2{100, 100},
            const PrSvgDrawOptions* PR_NULLABLE options = nullptr) {
        document = prDomParseXml(source, "image/svg+xml", nullptr);
        prCssEnable(document);
        prSvgEnable(document);
        REQUIRE(document != nullptr);
        PrSvgElement* root = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
        REQUIRE(root != nullptr);
        list = prDrawListCreate();
        prSvgElementDraw(root, viewport, options, list);
        commands = commandsOf(list);
        extent = viewport;
    }

    Drawing(const Drawing&) = delete;
    Drawing& operator=(const Drawing&) = delete;

    ~Drawing() {
        prDrawListRelease(list);
        prDomDocumentRelease(document);
    }

    // Returns the commands of `kind`.
    Array<const Drawn*> of(PrDrawCommandKind kind) const {
        Array<const Drawn*> found;
        for (const Drawn& command : commands)
            if (command.kind == kind)
                found.push(&command);
        return found;
    }

    // Returns the commands that fill, or that stroke.
    Array<const Drawn*> fills() const { return filtered(false); }
    Array<const Drawn*> strokes() const { return filtered(true); }

    // Returns the color of the pixel at `x`, `y` of the drawing rendered on the CPU over white, as "r g b".
    String pixel(uint32_t x, uint32_t y) const {
        uint32_t width = uint32_t(extent.x);
        uint32_t height = uint32_t(extent.y);
        Array<uint8_t> bytes(width * height * 4);
        prMemFill(bytes.data(), 255, bytes.count());
        PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
        prDrawRendererRenderPixels(renderer, list, PrPixels{bytes.data(), size_t(width) * 4, {width, height}},
            PrPixelFormat_Rgba8);
        prDrawRendererDestroy(renderer);
        const uint8_t* p = bytes.data() + (size_t(y) * width + x) * 4;
        return format("{} {} {}", p[0], p[1], p[2]);
    }

    // Returns the painting commands of `list` in order, as a drawing reads them.
    static Array<Drawn> commandsOf(const PrDrawList* list) {
        Array<Drawn> commands;
        Array<PrAffine2> transforms;
        transforms.push(prAffine2Identity());
        Array<float> opacities;
        opacities.push(1);
        uint32_t clips = 0;
        const PrDrawCommand* all = prDrawListCommands(list);
        for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
            const PrDrawCommand& c = all[i];
            switch (c.kind) {
                case PrDrawCommandKind_PushTransformAffine2:
                    transforms.push(prAffine2Concat(prAffine2FromDAffine2(c.affine), transforms.last()));
                    continue;
                case PrDrawCommandKind_PopTransform: transforms.pop(); continue;
                case PrDrawCommandKind_PushGroup: opacities.push(opacities.last() * c.group->opacity); continue;
                case PrDrawCommandKind_PopGroup: opacities.pop(); continue;
                case PrDrawCommandKind_PushClipRect:
                case PrDrawCommandKind_PushClipTargetRect:
                case PrDrawCommandKind_PushClipCornerRect:
                case PrDrawCommandKind_PushClipPath: ++clips; continue;
                case PrDrawCommandKind_PopClip: --clips; continue;
                case PrDrawCommandKind_PushMark:
                case PrDrawCommandKind_PopMark: continue;
                default: break;
            }
            Drawn d{};
            d.kind = c.kind;
            d.transform = transforms.last();
            d.color = prColorFromColor16(c.color);
            d.brushed = c.brush != nullptr;
            d.brush = c.brush;
            d.antialias = c.antialias;
            d.opacity = opacities.last();
            d.clips = clips;
            d.rule = c.rule;
            PrRect local{};
            switch (c.kind) {
                case PrDrawCommandKind_Rect: local = c.rect; break;
                case PrDrawCommandKind_CornerRect:
                    local = c.cornerRect->rect;
                    d.radii = c.cornerRect->corners[0].radii;
                    break;
                case PrDrawCommandKind_Ellipse:
                    local = PrRect{
                        c.ellipse.center.x - c.ellipse.radii.x, c.ellipse.center.y - c.ellipse.radii.y,
                        c.ellipse.center.x + c.ellipse.radii.x, c.ellipse.center.y + c.ellipse.radii.y,
                    };
                    d.radii = c.ellipse.radii;
                    break;
                case PrDrawCommandKind_Line:
                    local = PrRect{
                        prF32Min(c.line.start.x, c.line.end.x), prF32Min(c.line.start.y, c.line.end.y),
                        prF32Max(c.line.start.x, c.line.end.x), prF32Max(c.line.start.y, c.line.end.y),
                    };
                    break;
                case PrDrawCommandKind_Path: {
                    // The bounds of the points, which a path with no area, such as a line, has too.
                    const PrVec2* points = prPathPoints(c.path);
                    for (uint32_t p = 0; p < prPathPointCount(c.path); ++p) {
                        PrVec2 at = points[p];
                        if (p == 0)
                            local = PrRect{at.x, at.y, at.x, at.y};
                        else
                            local = PrRect{
                                prF32Min(local.left, at.x), prF32Min(local.top, at.y),
                                prF32Max(local.right, at.x), prF32Max(local.bottom, at.y),
                            };
                    }
                    const PrPathVerb* verbs = prPathVerbs(c.path);
                    for (uint32_t v = 0; v < prPathVerbCount(c.path); ++v)
                        d.verbs.push(verbs[v]);
                    break;
                }
                case PrDrawCommandKind_Glyphs: d.glyphs = c.glyphs; break;
                default: break;
            }
            d.bounds = mapped(d.transform, local);
            if (c.stroke != nullptr) {
                d.stroked = true;
                d.stroke = c.stroke->shape;
                d.nonScaling = c.stroke->nonScaling;
                for (uint32_t k = 0; k < d.stroke.dashCount; ++k)
                    d.dashes.push(d.stroke.dashes[k]);
                d.stroke.dashes = nullptr;
            }
            commands.push(move(d));
        }
        return commands;
    }

    // Returns the order in which the commands fill and stroke, as `f` for a fill and `s` for a stroke.
    String order() const {
        String out;
        for (const Drawn& command : commands)
            out.append(command.stroked ? 's' : 'f');
        return out;
    }

private:
    // Returns the bounds of the corners of `rect` mapped through `transform`, which keeps a rectangle with no area.
    static PrRect mapped(PrAffine2 transform, PrRect rect) {
        PrVec2 corners[] = {
            {rect.left, rect.top}, {rect.right, rect.top}, {rect.left, rect.bottom},
            {rect.right, rect.bottom},
        };
        PrRect out{};
        for (uint32_t i = 0; i < 4; ++i) {
            PrVec2 at = prAffine2MapPoint(transform, corners[i]);
            if (i == 0)
                out = PrRect{at.x, at.y, at.x, at.y};
            else
                out = PrRect{
                    prF32Min(out.left, at.x), prF32Min(out.top, at.y), prF32Max(out.right, at.x),
                    prF32Max(out.bottom, at.y),
                };
        }
        return out;
    }

    Array<const Drawn*> filtered(bool stroked) const {
        Array<const Drawn*> found;
        for (const Drawn& command : commands)
            if (command.stroked == stroked)
                found.push(&command);
        return found;
    }

};

// Returns true if two numbers are within a thousandth of each other.
inline bool near(float a, float b, float tolerance = 1e-3f) {
    return prF32Abs(a - b) <= tolerance;
}

// Returns true if two rectangles are within `tolerance` on each side. Reports both if they are not.
inline bool near(PrRect a, PrRect b, float tolerance = 1e-3f) {
    bool same = near(a.left, b.left, tolerance) && near(a.top, b.top, tolerance) && near(a.right, b.right, tolerance) &&
        near(a.bottom, b.bottom, tolerance);
    if (!same)
        MESSAGE("{" << a.left << ", " << a.top << ", " << a.right << ", " << a.bottom << "} is not {" << b.left <<
            ", " << b.top << ", " << b.right << ", " << b.bottom << "}");
    return same;
}

// Returns the angle that a transform turns the x-axis to, in degrees.
inline float angleOf(PrAffine2 transform) {
    return prF32Atan2(transform.b, transform.a) * 180 / 3.14159265f;
}

// Returns the scale of a transform along its x-axis.
inline float scaleOf(PrAffine2 transform) {
    return prF32Sqrt(transform.a * transform.a + transform.b * transform.b);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
