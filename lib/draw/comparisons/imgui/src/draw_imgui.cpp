// The Dear ImGui draw backend, which records a list through the stock ImDrawList and draws it with Dear ImGui's
// renderer for the device: Metal or Direct3D 12.
//
// Preparing runs a Dear ImGui frame. The frame's background draw list receives the list's commands, each through the
// matching ImDrawList call, using ImGui's own tessellation and stroking. Geometry is mapped to the target's pixels
// during recording, since ImDrawList has no transform. A command that ImDrawList has no call for is left out. The
// backend shows what the stock ImDrawList draws, not what it could draw with more. Encoding passes the frame to the
// renderer in the host's pass. This source records the frame, and the platform's source uploads images and draws.
//
// Glyphs are the exception, and a hack. Dear ImGui's atlas holds fonts that it maps from code points, but the list's
// glyphs are already shaped. The atlas loads fonts with Dear ImGui's FreeType loader, compiled here so that its one
// code point lookup reads the code point as a glyph id. Each of the list's fonts is added to the atlas. A variable
// instance is a separate font, with its axes set on the face that the loader opens. Dear ImGui then rasterizes, bakes,
// packs and uploads the glyphs in the same way as its own glyphs, and a glyph run draws as quads from the atlas.

#include "pr/draw_imgui.h"
#include "draw/comparisons/imgui/src/draw_imgui.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/draw_commands.h"
#include "pr/draw_glyph.h"
#include "pr/draw_image.h"
#include "pr/draw_lower.h"
#include "pr/draw_projection.h"
#include "pr/draw_transform.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/path_measure.h"
#include "pr/vec.h"

#include "draw/comparisons/imgui/src/draw_imgui_gpu.h"

#include "freetype/freetype.h"
#include "freetype/ftmm.h"
#include "imgui.h"
#include "imgui_internal.h"
// Dear ImGui's FreeType loader, with its one code point lookup changed to take the code point as a glyph id.
#define FT_Get_Char_Index(face, code) ((code) < FT_ULong((face)->num_glyphs) ? FT_UInt(code) : 0u)
#include "misc/freetype/imgui_freetype.cpp"
#undef FT_Get_Char_Index

namespace pr_impl {

namespace {

// The maximum distance of a flattened curve from the curve, in the target's pixels.
constexpr float kTolerance = 0.25f;

// The state of one prepare as it walks the commands.
struct Walk {
    ImGuiBackend* imgui;
    ImDrawList* draw;
    Array<PrDrawTransform> transforms;
    Array<ImVec4> clips;            // the scissor in force, innermost last
    Array<bool> projected;          // for each open group, true if it pushed a transform and a scissor
    PrPath* shape;
    PrPathPolyline flat;
    PrPathPolyline outline;
    Array<ImVec2> points;
};

ImU32 colorOf(PrColor c) {
    auto byte = [](float v) { return uint32_t(clampF32(v, 0, 1) * 255 + 0.5f); };
    return IM_COL32(byte(c.r), byte(c.g), byte(c.b), byte(c.a));
}

ImVec2 vec(PrVec2 p) {
    return ImVec2(p.x, p.y);
}

// ---------------------------------------------------------------------------------------------------------------------
// Paths

// Calls `each` with every contour of the polyline in target pixels, as points in `w.points`, and with whether the
// contour is closed.
template <typename Each>
void eachContour(Walk& w, const PrPathPolyline& flat, Each each) {
    for (uint32_t c = 0; c < flat.contourCount; ++c) {
        uint32_t start = flat.contours[c];
        uint32_t end = prPathPolylineContourEnd(&flat, c);
        bool closed = prPathPolylineContourIsClosed(&flat, c);
        // Coincident points are one point. Dear ImGui closes a closed contour itself, so a last point that repeats its
        // first is dropped.
        w.points.clear();
        for (uint32_t i = start; i < end; ++i) {
            ImVec2 p = vec(flat.points[i]);
            if (w.points.count == 0 || p.x != w.points.last().x || p.y != w.points.last().y)
                w.points.push(p);
        }
        if (closed && w.points.count > 1 && w.points.last().x == w.points.data[0].x &&
                w.points.last().y == w.points.data[0].y)
            w.points.pop();
        if (w.points.count >= 2)
            each(closed);
    }
}

// Fills each contour of the polyline, in target pixels, as a separate polygon. Dear ImGui fills neither holes nor
// overlaps by a fill rule.
void fillDevice(Walk& w, const PrPathPolyline& flat, ImU32 color) {
    eachContour(w, flat, [&](bool) {
        if (w.points.count >= 3)
            w.draw->AddConcavePolyFilled(w.points.data, int(w.points.count), color);
    });
}

// Fills each contour of the path, mapped by the transform and flattened, as a separate polygon.
void fillPath(Walk& w, const PrPath* path, PrAffine2 transform, ImU32 color) {
    prPathPolylineClear(&w.flat);
    prPathFlatten(path, transform, kTolerance, PrPathFlattening_Inside, &w.flat);
    fillDevice(w, w.flat, color);
}

// Returns twice the signed area of the contour in `w.points`. It is positive if the contour turns clockwise on the
// y-down target.
float signedArea(const Walk& w) {
    float area = 0;
    for (uint32_t i = 0; i < w.points.count; ++i) {
        ImVec2 a = w.points.data[i];
        ImVec2 b = w.points.data[(i + 1) % w.points.count];
        area += a.x * b.y - b.x * a.y;
    }
    return area;
}

// Returns Dear ImGui's stroke position for a contour of the stroke. An open contour is stroked on its center. A closed
// contour uses the side that the stroke's alignment names, which Dear ImGui measures against the contour's direction.
// Dear ImGui's inside is the interior of a clockwise contour.
ImDrawFlags strokePosition(const Walk& w, const PrDrawStroke* stroke, bool closed) {
    if (!closed || stroke->align == PrDrawStrokeAlign_Center)
        return ImDrawFlags_StrokeCenter;
    bool clockwise = signedArea(w) > 0;
    bool inside = stroke->align == PrDrawStrokeAlign_Inside;
    return inside == clockwise ? ImDrawFlags_StrokeInside : ImDrawFlags_StrokeOutside;
}

// Strokes each contour with Dear ImGui's own stroker, at the stroke's width in the target's pixels and at the position
// that its alignment sets. It uses ImGui's joins and caps, and draws no dashes.
void strokePath(Walk& w, const PrPath* path, PrAffine2 transform, const PrDrawStroke* stroke, ImU32 color) {
    float width = stroke->shape.width;
    if (!(width > 0))
        return;
    float thickness = stroke->nonScaling ? width : width * prAffine2MaxScale(transform);
    prPathPolylineClear(&w.flat);
    prPathFlatten(path, transform, kTolerance, PrPathFlattening_Inside, &w.flat);
    eachContour(w, w.flat, [&](bool closed) {
        ImDrawFlags flags = strokePosition(w, stroke, closed);
        if (closed)
            flags |= ImDrawFlags_Closed;
        w.draw->AddPolyline(w.points.data, int(w.points.count), color, thickness, flags);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

void drawImageRects(Walk& w, const PrDrawSource& source, PrColor color, const PrDrawImageRect* rects, uint32_t count);

// Draws a rectangle filled with an image brush in the command's space, without rotation, skew or reflection, as the
// image's tile clipped to the rectangle. The tile's surroundings are left out. Returns false for any other brush.
bool drawImageBrushRect(Walk& w, const PrDrawCommand& c) {
    const PrDrawBrush* brush = c.brush;
    if (brush == nullptr || brush->kind != PrDrawBrushKind_Image || brush->space != PrDrawBrushSpace_User)
        return false;
    const PrAffine2& m = brush->transform;
    if (!(m.b == 0 && m.c == 0 && m.a > 0 && m.d > 0))
        return false;
    const PrDrawSource& source = brush->image.source;
    PrRect tile = brush->image.tiles.tile;
    PrRect placed = prAffine2MapRect(brush->transform, tile);
    PrRect visible{
        maxF32(placed.left, c.rect.left), maxF32(placed.top, c.rect.top),
        minF32(placed.right, c.rect.right), minF32(placed.bottom, c.rect.bottom),
    };
    if (!(visible.right > visible.left && visible.bottom > visible.top) || !(placed.right > placed.left) ||
            !(placed.bottom > placed.top))
        return true;
    // The visible part of the tile, in the source's texels from its origin.
    float width = prRectWidth(source.rect) / prRectWidth(placed);
    float height = prRectHeight(source.rect) / prRectHeight(placed);
    PrDrawImageRect rect{
        PrRect{
            (visible.left - placed.left) * width, (visible.top - placed.top) * height,
            (visible.right - placed.left) * width, (visible.bottom - placed.top) * height,
        },
        visible,
    };
    drawImageRects(w, source, c.color, &rect, 1);
    return true;
}

// Draws a shape. The command paints its color. A linear brush on a rectangle instead paints the corner colors of Dear
// ImGui's one gradient, and an image brush paints the image. Any other brush is left out.
void drawShape(Walk& w, const PrDrawCommand& c) {
    ImU32 color = colorOf(c.color);
    PrAffine2 t = w.transforms.last().affine;
    // An image's quads accept any transform of the plane.
    if (c.stroke == nullptr && c.kind == PrDrawCommandKind_Rect && drawImageBrushRect(w, c))
        return;
    if (c.stroke == nullptr && prAffine2IsAxisAligned(t)) {
        if (c.kind == PrDrawCommandKind_Rect) {
            PrRect r = prAffine2MapRect(t, c.rect);
            if (c.brush != nullptr && c.brush->kind == PrDrawBrushKind_Linear &&
                    c.brush->space == PrDrawBrushSpace_User) {
                // The ramp's colors at the corners, at the gradient's parameter for each corner.
                Array<PrDrawRampPiece> pieces{};
                Array<PrColor> ramp{};
                pieces.resize(prDrawGradientRampMaxPieces(c.brush->stopCount));
                ramp.resize(prDrawGradientRampMaxColors(c.brush->stopCount, 256));
                pieces.resize(prDrawGradientRamp(c.brush, c.color, PrColorSpace_Srgb, 256, pieces.data, ramp.data));
                auto at = [&](PrVec2 p) {
                    PrVec2 brushPoint = prAffine2MapPoint(c.brush->transform, p);
                    float param = 0;
                    prDrawGradientParameter(c.brush, brushPoint, &param);
                    PrColor k{};
                    if (prDrawGradientExtend(c.brush->extend, &param))
                        k = prDrawRampColor(&pieces.data[prDrawRampPieceAt(pieces.data, param, 0)], ramp.data, param);
                    float a = k.a;
                    return colorOf(PrColor{a > 0 ? k.r / a : 0, a > 0 ? k.g / a : 0, a > 0 ? k.b / a : 0, a});
                };
                // The command's points at the target's corners. A reflection or a swap of the axes moves them.
                PrAffine2 back{};
                if (prAffine2Invert(t, &back)) {
                    auto corner = [&](float x, float y) { return at(prAffine2MapPoint(back, PrVec2{x, y})); };
                    w.draw->AddRectFilledMultiColor(ImVec2(r.left, r.top), ImVec2(r.right, r.bottom), corner(r.left, r.top),
                        corner(r.right, r.top), corner(r.right, r.bottom), corner(r.left, r.bottom));
                }
                pieces.reset();
                ramp.reset();
                return;
            }
            w.draw->AddRectFilled(ImVec2(r.left, r.top), ImVec2(r.right, r.bottom), color);
            return;
        }
        if (c.kind == PrDrawCommandKind_CornerRect) {
            // Dear ImGui's rounded rectangle has one circular radius for every round corner.
            const PrCornerRect& shape = *c.cornerRect;
            float radius = shape.corners[0].radii.x;
            bool uniform = true;
            for (const PrCorner& corner : shape.corners)
                uniform =
                    uniform && corner.radii.x == radius && corner.radii.y == radius && (corner.shape == 1 || !(radius > 0));
            if (uniform) {
                PrRect r = prAffine2MapRect(t, shape.rect);
                w.draw->AddRectFilled(ImVec2(r.left, r.top), ImVec2(r.right, r.bottom), color,
                    radius * prAffine2MaxScale(t));
                return;
            }
        }
        if (c.kind == PrDrawCommandKind_Ellipse && prAffine2IsSimilarity(t)) {
            float scale = prAffine2MaxScale(t);
            float turn = atan2F32(t.b, t.a);
            w.draw->AddEllipseFilled(vec(prAffine2MapPoint(t, c.ellipse.center)),
                ImVec2(c.ellipse.radii.x * scale, c.ellipse.radii.y * scale), color, c.ellipse.rotation + turn);
            return;
        }
    }
    prPathClear(w.shape);
    prDrawShapePath(&c, w.shape);
    if (c.stroke != nullptr)
        strokePath(w, w.shape, t, c.stroke, color);
    else
        fillPath(w, w.shape, t, color);
}

struct BorderDraw {
    Walk* w;
    const PrDrawCommand* command;
};

// Draws a border's pieces as polygons in each side's color. Its brushes and the cuts between sides are left out.
void drawBorderPiece(void* PR_NULLABLE context, const PrDrawBorderPiece* piece,
        const PrPathPolyline* shape, const PrPathPolyline* PR_NULLABLE) {
    BorderDraw& d = *static_cast<BorderDraw*>(context);
    Walk& w = *d.w;
    PrAffine2 device = w.transforms.last().affine;
    prPathPolylineClear(&w.flat);
    prPathPolylineAddPolyline(&w.flat, shape, device);
    fillDevice(w, w.flat, colorOf(piece->color));
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Draws the source's texels over each rectangle, under the current transform. Dear ImGui tints them with the vertex
// color times the texels, as they are stored. A list source is left out.
void drawImageRects(Walk& w, const PrDrawSource& source, PrColor color, const PrDrawImageRect* rects, uint32_t count) {
    if (source.kind == PrDrawSourceKind_List)
        return;
    ImTextureID texture = ImTextureID_Invalid;
    PrUVec2 extent{};
    if (source.kind == PrDrawSourceKind_Image) {
        texture = imguiImageTexture(w.imgui, source.image, &extent);
    } else if (source.texture != 0) {
        texture = imguiHostTexture(w.imgui, source.texture);
        extent = source.extent;
    }
    if (texture == ImTextureID_Invalid || extent.x == 0 || extent.y == 0)
        return;
    ImTextureRef ref(texture);
    float width = float(extent.x);
    float height = float(extent.y);
    PrAffine2 t = w.transforms.last().affine;
    ImU32 packed = colorOf(color);
    for (uint32_t i = 0; i < count; ++i) {
        PrRect s = rects[i].source;
        s = PrRect{
            source.rect.left + s.left, source.rect.top + s.top, source.rect.left + s.right,
            source.rect.top + s.bottom,
        };
        const PrRect& d = rects[i].destination;
        ImVec2 uv0(s.left / width, s.top / height);
        ImVec2 uv1(s.right / width, s.bottom / height);
        w.draw->AddImageQuad(ref, vec(prAffine2MapPoint(t, {d.left, d.top})),
            vec(prAffine2MapPoint(t, {d.right, d.top})), vec(prAffine2MapPoint(t, {d.right, d.bottom})),
            vec(prAffine2MapPoint(t, {d.left, d.bottom})), uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y), packed);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Glyphs

FT_Face faceOf(ImFont* font) {
    return static_cast<ImGui_ImplFreeType_FontSrcData*>(font->Sources[0]->FontLoaderData)->FtFace;
}

// Returns the font in the atlas, which is added once and kept for as long as renders use it. Returns null if FreeType
// cannot read the font. Its synthetic bold is the loader's, which is FreeType's fixed emboldening. Its color glyphs are
// drawn only as far as FreeType draws them.
ImFont* PR_NULLABLE fontOf(ImGuiBackend* imgui, const PrFont* font) {
    uint64_t id = prFontId(font);
    for (uint32_t i = 0; i < imgui->fonts.count; ++i) {
        if (imgui->fonts.data[i].id == id) {
            imgui->fonts.data[i].lastUsed = imgui->render;
            return imgui->fonts.data[i].added;
        }
    }
    ImFontConfig config;
    PrByteView bytes = prFontData(font);
    config.FontData = const_cast<uint8_t*>(bytes.data);
    config.FontDataSize = int(bytes.size);
    config.FontDataOwnedByAtlas = false;
    config.FontNo = prFontIndex(font);
    config.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor;
    if (prFontEmbolden(font) > 0)
        config.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_Bold;
    ImFont* added = ImGui::GetIO().Fonts->AddFont(&config);
    if (added != nullptr) {
        prFontRetain(font);
        // SVG glyphs need a renderer that Dear ImGui builds only with plutosvg or lunasvg, and this build leaves both
        // out. Such a font draws its outlines instead.
        FT_Face face = faceOf(added);
        if (FT_HAS_SVG(face))
            static_cast<ImGui_ImplFreeType_FontSrcData*>(added->Sources[0]->FontLoaderData)
                ->LoadFlags &= ~FT_Int32(FT_LOAD_COLOR);
        // The instance's axes, set before the loader rasterizes any glyph.
        FT_Fixed coordinates[32];
        uint32_t axes = prFontAxisCount(font) < 32 ? prFontAxisCount(font) : 32;
        for (uint32_t a = 0; a < axes; ++a)
            coordinates[a] = FT_Fixed(prFontAxisValue(font, a) * 65536.0f);
        if (axes != 0)
            FT_Set_Var_Design_Coordinates(face, axes, coordinates);
    }
    imgui->fonts.push(ImGuiCachedFont{id, added != nullptr ? font : nullptr, added, imgui->render});
    return added;
}

// Draws the run's glyphs as quads from the atlas. Each glyph is baked at the size that Dear ImGui assigns to the em's
// pixel size on the target. Dear ImGui's sizes are whole pixels from ascender to descender. Each quad is mapped by the
// glyph's transform and colored. A color glyph uses only the color's alpha, which is how Dear ImGui draws its own color
// glyphs. A brush and a stroke are left out.
void drawGlyphs(Walk& w, const PrDrawCommand& c) {
    const PrFontGlyphRun& run = *c.glyphs;
    if (c.stroke != nullptr)
        return;
    ImFont* font = fontOf(w.imgui, run.font);
    if (font == nullptr)
        return;
    FT_Face face = faceOf(font);
    // Dear ImGui sizes only scalable fonts, not fonts with only bitmap strikes.
    if (!FT_IS_SCALABLE(face))
        return;
    float heightPerEm = float(face->ascender - face->descender) / float(face->units_per_EM);
    ImU32 color = colorOf(c.color);
    float adjust = prFontSizeAdjust(run.font);
    PrAffine2 device = w.transforms.last().affine;
    for (uint32_t i = 0; i < run.glyphCount; ++i) {
        uint16_t id = run.glyphs[i].id;
        float perEm = prDrawGlyphPixelsPerEm(&run, i, device) * adjust;
        if (!(perEm > 0))
            continue;
        ImFontBaked* baked = font->GetFontBaked(clampF32(perEm * heightPerEm, 1, IMGUI_FONT_SIZE_MAX));
        if (baked == nullptr)
            continue;
        ImFontGlyph* glyph = baked->FindGlyphNoFallback(ImWchar(id));
        if (glyph == nullptr || !glyph->Visible)
            continue;
        // The atlas's current texture and the glyph's current UVs. Loading the glyph may have moved the atlas to a
        // larger texture.
        ImTextureRef atlas = font->OwnerAtlas->TexRef;
        // Maps the pixels of the baked box, about the origin on the baseline, to the font's ems at the scale that
        // FreeType used for the size. Then applies the glyph's placement and the target's transform.
        const FT_Size_Metrics& metrics = static_cast<ImGui_ImplFreeType_FontSrcBakedData*>(baked->FontLoaderDatas)
            ->FtSize->metrics;
        float unitsToPixels = float(face->units_per_EM) / (65536.0f * 64.0f);
        float toEmsX = adjust / (float(metrics.x_scale) * unitsToPixels);
        float toEmsY = adjust / (float(metrics.y_scale) * unitsToPixels);
        PrAffine2 box = prAffine2Concat(prAffine2Translate(PrVec2{0, -baked->Ascent}), prAffine2Scale(PrVec2{toEmsX, toEmsY}));
        PrAffine2 t = prAffine2Concat(box, prAffine2Concat(prDrawGlyphTransform(&run, i), device));
        auto at = [&](float x, float y) { return vec(prAffine2MapPoint(t, PrVec2{x, y})); };
        ImU32 shade = glyph->Colored ? (color | ~IM_COL32_A_MASK) : color;
        w.draw->AddImageQuad(atlas, at(glyph->X0, glyph->Y0), at(glyph->X1, glyph->Y0),
            at(glyph->X1, glyph->Y1), at(glyph->X0, glyph->Y1), ImVec2(glyph->U0, glyph->V0),
            ImVec2(glyph->U1, glyph->V0), ImVec2(glyph->U1, glyph->V1), ImVec2(glyph->U0, glyph->V1), shade);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

// Pushes a clip. A rectangle that stays axis-aligned under the transform becomes a scissor rectangle. Otherwise the
// current clip stays, and the shape is left out.
void pushClip(Walk& w, const PrDrawCommand& c) {
    ImVec4 clip = w.clips.last();
    PrRect rect{};
    bool scissor = false;
    if (c.kind == PrDrawCommandKind_PushClipTargetRect) {
        rect = c.rect;
        scissor = true;
    } else if (c.kind == PrDrawCommandKind_PushClipRect && prAffine2IsAxisAligned(w.transforms.last().affine)) {
        rect = prAffine2MapRect(w.transforms.last().affine, c.rect);
        scissor = true;
    }
    if (prDrawTransformDrawsNothing(&w.transforms.last(), &c)) {
        clip = ImVec4(0, 0, 0, 0);
    } else if (scissor) {
        clip = ImVec4(maxF32(clip.x, rect.left), maxF32(clip.y, rect.top), minF32(clip.z, rect.right),
            minF32(clip.w, rect.bottom));
        if (clip.z < clip.x || clip.w < clip.y)
            clip = ImVec4(0, 0, 0, 0);
    }
    w.clips.push(clip);
    w.draw->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w));
}

// Pushes a group. ImDrawList has no layers, so a group's contents draw in place without its effects. A projected
// group's contents draw through the projection's tangent at the center of the plane's visible part, within the scissor
// of where the plane lands. A projected group that draws nothing gets an empty scissor.
void pushGroup(Walk& w, const PrDrawCommand& c) {
    const PrDrawGroup* g = c.group;
    w.projected.push(g->projection != nullptr);
    if (g->projection == nullptr)
        return;
    const PrDrawTransform& x = w.transforms.last();
    ImVec4 clip = w.clips.last();
    PrDrawProjection p{};
    PrAffine2 tangent = prAffine2Identity();
    if (prDrawProjectionPlace(g->projection, x.affine, g->bounds, PrRect{clip.x, clip.y, clip.z, clip.w},
            float(kPrDrawRenderMaxSize), &p)) {
        tangent = prDrawProjectionTangent(&p);
        const PrRect& landed = p.targetBounds;
        clip = ImVec4(maxF32(clip.x, landed.left), maxF32(clip.y, landed.top), minF32(clip.z, landed.right),
            minF32(clip.w, landed.bottom));
    } else {
        clip = ImVec4(0, 0, 0, 0);
    }
    w.transforms.push(prDrawTransformFromAffine2(tangent));
    w.clips.push(clip);
    w.draw->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w));
}

void popGroup(Walk& w) {
    bool projected = w.projected.last();
    w.projected.pop();
    if (!projected)
        return;
    w.clips.pop();
    w.draw->PopClipRect();
    w.transforms.pop();
}

void runCommands(Walk& w, const PrDrawCommand* commands, uint32_t count) {
    PrDrawSnapped snapped;
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = *prDrawTransformSnap(&w.transforms.last(), &commands[i], &snapped);
        if (prDrawTransformDrawsNothing(&w.transforms.last(), &c)) {
            // A push still opens a scope that its pop closes.
            if (c.kind == PrDrawCommandKind_PushGroup)
                w.projected.push(false);
            if (!(c.kind >= PrDrawCommandKind_PushClipRect && c.kind <= PrDrawCommandKind_PushClipPath))
                continue;
        }
        switch (c.kind) {
            case PrDrawCommandKind_Clear: {
                ImVec4 clip = w.clips.last();
                w.draw->AddRectFilled(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), colorOf(c.color));
                break;
            }
            case PrDrawCommandKind_Border: {
                BorderDraw d{&w, &c};
                prDrawBorderLower(c.border, kTolerance, drawBorderPiece, &d);
                break;
            }
            case PrDrawCommandKind_Decoration: {
                PrAffine2 device = w.transforms.last().affine;
                float scale = prAffine2MaxScale(device);
                if (!(scale > 0))
                    break;
                prPathPolylineClear(&w.outline);
                prDrawDecorationOutline(c.decoration, kTolerance / scale, &w.outline);
                prPathPolylineClear(&w.flat);
                prPathPolylineAddPolyline(&w.flat, &w.outline, device);
                fillDevice(w, w.flat, colorOf(c.color));
                break;
            }
            case PrDrawCommandKind_NineSlice: {
                const PrRect& area = c.nineSlice.source.rect;
                PrDrawImageRect rects[64];
                uint32_t n = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{prRectWidth(area), prRectHeight(area)},
                    c.nineSlice.destination, rects, 64);
                drawImageRects(w, c.nineSlice.source, c.color, rects, n < 64 ? n : 64);
                break;
            }
            case PrDrawCommandKind_Glyphs:
                drawGlyphs(w, c);
                break;
            case PrDrawCommandKind_Embed:
                w.transforms.push(prDrawTransformMoved(&w.transforms.last(), c.embed.offset));
                runCommands(w, prDrawListCommands(c.embed.list), prDrawListCommandCount(c.embed.list));
                w.transforms.pop();
                break;
            case PrDrawCommandKind_PushTransformAffine2: {
                PrDrawTransform next = prDrawTransformPush(&w.transforms.last(), &c);
                w.transforms.push(next);
                break;
            }
            case PrDrawCommandKind_PopTransform:
                w.transforms.pop();
                break;
            case PrDrawCommandKind_PushClipRect:
            case PrDrawCommandKind_PushClipTargetRect:
            case PrDrawCommandKind_PushClipCornerRect:
            case PrDrawCommandKind_PushClipPath:
                pushClip(w, c);
                break;
            case PrDrawCommandKind_PopClip:
                w.clips.pop();
                w.draw->PopClipRect();
                break;
            case PrDrawCommandKind_DropShadow:
            case PrDrawCommandKind_InnerShadow:
            case PrDrawCommandKind_WritePixels:
                break;
            case PrDrawCommandKind_PushGroup:
                pushGroup(w, c);
                break;
            case PrDrawCommandKind_PopGroup:
                popGroup(w);
                break;
            default:
                drawShape(w, c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The backend

void removeFont(const ImGuiCachedFont& font) {
    if (font.added != nullptr)
        ImGui::GetIO().Fonts->RemoveFont(font.added);
    prFontRelease(font.font);
}

// Drops the fonts that the render did not use.
void dropUnusedFonts(ImGuiBackend* imgui) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < imgui->fonts.count; ++i) {
        if (imgui->fonts.data[i].lastUsed == imgui->render)
            imgui->fonts.data[kept++] = imgui->fonts.data[i];
        else
            removeFont(imgui->fonts.data[i]);
    }
    imgui->fonts.count = kept;
}

} // namespace

// Premultiplied as the list's images are read, then converted to straight alpha.
void imguiImagePixels(const PrDrawImage* image, PrPixels pixels) {
    prDrawImageWritePixels(image, pixels, PrPixelFormat_Rgba8, PrIVec2{0, 0});
    for (uint32_t y = 0; y < pixels.extent.y; ++y) {
        uint8_t* row = pixels.data + y * pixels.stride;
        for (uint32_t x = 0; x < pixels.extent.x * 4; x += 4) {
            uint32_t a = row[x + 3];
            for (uint32_t k = 0; k < 3; ++k)
                row[x + k] = uint8_t(a == 0 ? 0 : (row[x + k] * 255 + a / 2) / a);
        }
    }
}

void imguiFrameClear(ImGuiFrame* frame) {
    for (ImDrawList* list : frame->data.CmdLists)
        IM_DELETE(list);
    frame->data.CmdLists.clear();
}

void imguiRecord(ImGuiBackend* imgui, ImGuiFrame* frame, const PrDrawList* list, const PrDrawTarget* target) {
    float width = float(target->extent.x);
    float height = float(target->extent.y);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    Walk w{};
    w.imgui = imgui;
    w.draw = ImGui::GetBackgroundDrawList();
    w.transforms.push(prDrawTransformFromAffine2(prAffine2Identity()));
    w.clips.push(ImVec4(0, 0, width, height));
    w.shape = prPathCreate();
    runCommands(w, prDrawListCommands(list), prDrawListCommandCount(list));
    prPathDestroy(w.shape);
    prPathPolylineReset(&w.flat);
    prPathPolylineReset(&w.outline);
    w.transforms.reset();
    w.clips.reset();
    w.projected.reset();
    w.points.reset();
    ImGui::Render();
    const ImDrawData* drawn = ImGui::GetDrawData();
    frame->data = *drawn;
    for (ImDrawList*& copy : frame->data.CmdLists)
        copy = copy->CloneOutput();
    dropUnusedFonts(imgui);
}

void imguiContextDestroy(ImGuiBackend* imgui) {
    ImGui::DestroyContext(imgui->context);
    for (uint32_t i = 0; i < imgui->fonts.count; ++i)
        prFontRelease(imgui->fonts.data[i].font);
    imgui->fonts.reset();
}

} // namespace pr_impl

using namespace pr_impl;

bool prDrawImGuiCreate(PrGpuDevice* device, PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
    ImGuiBackend* imgui = imguiDeviceCreate(device);
    if (imgui == nullptr) {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
        return false;
    }
    imgui->context = context;
    ImGui::SetCurrentContext(previous);
    *out = imgui;
    return true;
}
