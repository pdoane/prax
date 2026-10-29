// The fixture of the SVG tests: a standalone document drawn into a list, and its painting commands as a test reads
// them.

#pragma once

#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/draw_commands.h"
#include "pr/draw_cpu.h"
#include "pr/draw_renderer.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"

#include "doctest/doctest.h"

#include <cmath>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns `body` as the content of an outermost svg element in SVG's namespace, with xlink's declared.
inline std::string svgDocument(const std::string& body) {
    return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink'>" + body + "</svg>";
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
    std::vector<float> dashes;

    PrPathFillRule rule;                      // Path
    PrVec2 radii;                             // Ellipse, and the first corner of a CornerRect
    std::vector<PrPathVerb> verbs;            // Path
    const PrFontGlyphRun* PR_NULLABLE glyphs; // Glyphs, valid while the drawing lives
};

// A document drawn into a list, and the painting commands of the list in order.
struct Drawing {
    PrDomDocument* document = nullptr;
    PrDrawList* list = nullptr;
    std::vector<Drawn> commands;
    PrVec2 extent{};

    // Draws the document `source` in a viewport of `viewport`, with `options`, or the defaults if null.
    explicit Drawing(const std::string& source, PrVec2 viewport = PrVec2{100, 100},
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
    std::vector<const Drawn*> of(PrDrawCommandKind kind) const {
        std::vector<const Drawn*> found;
        for (const Drawn& command : commands)
            if (command.kind == kind)
                found.push_back(&command);
        return found;
    }

    // Returns the commands that fill, or that stroke.
    std::vector<const Drawn*> fills() const { return filtered(false); }
    std::vector<const Drawn*> strokes() const { return filtered(true); }

    // Returns the color of the pixel at `x`, `y` of the drawing rendered on the CPU over white, as "r g b".
    std::string pixel(uint32_t x, uint32_t y) const {
        uint32_t width = uint32_t(extent.x);
        uint32_t height = uint32_t(extent.y);
        std::vector<uint8_t> bytes(size_t(width) * height * 4, 255);
        PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
        prDrawRendererRenderPixels(renderer, list, PrPixels{bytes.data(), size_t(width) * 4, {width, height}},
                                   PrPixelFormat_Rgba8);
        prDrawRendererDestroy(renderer);
        const uint8_t* p = bytes.data() + (size_t(y) * width + x) * 4;
        return std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]);
    }

    // Returns the painting commands of `list` in order, as a drawing reads them.
    static std::vector<Drawn> commandsOf(const PrDrawList* list) {
        std::vector<Drawn> commands;
        std::vector<PrAffine2> transforms{prAffine2Identity()};
        std::vector<float> opacities{1};
        uint32_t clips = 0;
        const PrDrawCommand* all = prDrawListCommands(list);
        for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
            const PrDrawCommand& c = all[i];
            switch (c.kind) {
                case PrDrawCommandKind_PushTransformAffine2:
                    transforms.push_back(prAffine2Concat(prAffine2FromDAffine2(c.affine), transforms.back()));
                    continue;
                case PrDrawCommandKind_PopTransform: transforms.pop_back(); continue;
                case PrDrawCommandKind_PushGroup: opacities.push_back(opacities.back() * c.group->opacity); continue;
                case PrDrawCommandKind_PopGroup: opacities.pop_back(); continue;
                case PrDrawCommandKind_PushClipRect:
                case PrDrawCommandKind_PushClipTargetRect:
                case PrDrawCommandKind_PushClipCornerRect:
                case PrDrawCommandKind_PushClipPath: ++clips; continue;
                case PrDrawCommandKind_PopClip: --clips; continue;
                default: break;
            }
            Drawn d{};
            d.kind = c.kind;
            d.transform = transforms.back();
            d.color = prColorFromColor16(c.color);
            d.brushed = c.brush != nullptr;
            d.brush = c.brush;
            d.antialias = c.antialias;
            d.opacity = opacities.back();
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
                    local = PrRect{c.ellipse.center.x - c.ellipse.radii.x, c.ellipse.center.y - c.ellipse.radii.y,
                                   c.ellipse.center.x + c.ellipse.radii.x, c.ellipse.center.y + c.ellipse.radii.y};
                    d.radii = c.ellipse.radii;
                    break;
                case PrDrawCommandKind_Line:
                    local = PrRect{std::fmin(c.line.start.x, c.line.end.x), std::fmin(c.line.start.y, c.line.end.y),
                                   std::fmax(c.line.start.x, c.line.end.x), std::fmax(c.line.start.y, c.line.end.y)};
                    break;
                case PrDrawCommandKind_Path: {
                    // The bounds of the points, which a path with no area, such as a line, has too.
                    const PrVec2* points = prPathPoints(c.path);
                    for (uint32_t p = 0; p < prPathPointCount(c.path); ++p) {
                        PrVec2 at = points[p];
                        local = p == 0 ? PrRect{at.x, at.y, at.x, at.y}
                                       : PrRect{std::fmin(local.left, at.x), std::fmin(local.top, at.y),
                                                std::fmax(local.right, at.x), std::fmax(local.bottom, at.y)};
                    }
                    const PrPathVerb* verbs = prPathVerbs(c.path);
                    for (uint32_t v = 0; v < prPathVerbCount(c.path); ++v)
                        d.verbs.push_back(verbs[v]);
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
                    d.dashes.push_back(d.stroke.dashes[k]);
                d.stroke.dashes = nullptr;
            }
            commands.push_back(d);
        }
        return commands;
    }

    // Returns the order in which the commands fill and stroke, as `f` for a fill and `s` for a stroke.
    std::string order() const {
        std::string out;
        for (const Drawn& command : commands)
            out += command.stroked ? 's' : 'f';
        return out;
    }

private:
    // Returns the bounds of the corners of `rect` mapped through `transform`, which keeps a rectangle with no area.
    static PrRect mapped(PrAffine2 transform, PrRect rect) {
        PrVec2 corners[] = {{rect.left, rect.top}, {rect.right, rect.top}, {rect.left, rect.bottom},
                            {rect.right, rect.bottom}};
        PrRect out{};
        for (uint32_t i = 0; i < 4; ++i) {
            PrVec2 at = prAffine2MapPoint(transform, corners[i]);
            out = i == 0 ? PrRect{at.x, at.y, at.x, at.y}
                         : PrRect{std::fmin(out.left, at.x), std::fmin(out.top, at.y), std::fmax(out.right, at.x),
                                  std::fmax(out.bottom, at.y)};
        }
        return out;
    }

    std::vector<const Drawn*> filtered(bool stroked) const {
        std::vector<const Drawn*> found;
        for (const Drawn& command : commands)
            if (command.stroked == stroked)
                found.push_back(&command);
        return found;
    }

};

// Returns true if two numbers are within a thousandth of each other.
inline bool near(float a, float b, float tolerance = 1e-3f) {
    return std::fabs(a - b) <= tolerance;
}

// Returns true if two rectangles are within `tolerance` on each side. Reports both if they are not.
inline bool near(PrRect a, PrRect b, float tolerance = 1e-3f) {
    bool same = near(a.left, b.left, tolerance) && near(a.top, b.top, tolerance) && near(a.right, b.right, tolerance)
                && near(a.bottom, b.bottom, tolerance);
    if (!same)
        MESSAGE("{" << a.left << ", " << a.top << ", " << a.right << ", " << a.bottom << "} is not {" << b.left
                    << ", " << b.top << ", " << b.right << ", " << b.bottom << "}");
    return same;
}

// Returns the angle that a transform turns the x-axis to, in degrees.
inline float angleOf(PrAffine2 transform) {
    return std::atan2(transform.b, transform.a) * 180 / 3.14159265f;
}

// Returns the scale of a transform along its x-axis.
inline float scaleOf(PrAffine2 transform) {
    return std::sqrt(transform.a * transform.a + transform.b * transform.b);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
