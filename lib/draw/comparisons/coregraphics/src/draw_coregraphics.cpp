// The CoreGraphics draw backend, which renders with Quartz 2D into pixels in memory, and with CoreText for native text.
//
// The list's transforms are Quartz's transforms. A push saves the graphics state and concatenates onto the context's
// transform, and its pop restores the state. Geometry goes to Quartz in the command's space, and Quartz fills and
// strokes it. A brush draws in its own space under a clip to the shape. Clips and groups save the graphics state at
// their push and restore it at their pop. A group with an effect is a transparency layer. An operator that changes what
// lies outside a shape draws into a layer the size of the clip, which is transparent outside the shape when it
// composites.

#include "pr/draw_coregraphics.h"

#include "pr/alloc.h"
#include "pr/draw_commands.h"
#include "pr/draw_filter.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_projection.h"
#include "pr/draw_transform.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "draw/backends/shared/src/draw_shared_memory.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

namespace pr_impl {

namespace {

// The maximum distance of a flattened curve from the true shape, in target pixels.
constexpr float kTolerance = 0.05f;

// The colors per unit of a gradient's parameter in a ramp's pieces that mix nonlinearly.
constexpr uint32_t kRampResolution = 256;

// The maximum number of tiles that an image brush draws. A pattern that needs more is too fine to matter.
constexpr uint32_t kMaxTiles = 16384;

struct CachedImage {
    uint64_t id;
    CGImageRef image;
    uint64_t lastUsed;
};

struct CachedFont {
    uint64_t id;
    CGFontRef PR_NULLABLE font;
    uint64_t lastUsed;
};

// A CoreText font for a Quartz font at a size, cached for the render that created it.
struct SizedFont {
    uint64_t id;
    float size;
    CTFontRef font;
};

struct CgBackend : PrDrawRenderer {
    bool ownsDevice;                    // true if the renderer created its device, and frees it
    PrDrawCoreGraphicsOptions options;
    CGColorSpaceRef srgb;
    CGColorSpaceRef extended;
    CGColorSpaceRef gray;
    RawArray<CachedImage> images;
    KeyIndex imageIndex;                // `images`, by id
    PrDrawGlyphCache* glyphs;
    RawArray<CachedFont> fonts;
    RawArray<SizedFont> sized;
    uint64_t render;
};

// A push of a transform, clip or group, whether it saved the graphics state or began a layer, and whether it is a
// transform that collapses or is pushed inside one that does.
struct Push {
    PrDrawCommandKind kind;
    bool saved;
    bool layered;
    bool collapsed;
};

// The state of one render as it walks the commands. A nested render, such as a list brush's render, has its own state.
struct Renderer {
    CgBackend* cg;
    CGContextRef context;
    CGAffineTransform target;                   // the context's transform for target pixels
    const PrPixels* PR_NULLABLE pixels;         // the target's pixels, which WritePixels writes
    PrPixelFormat format;                       // the target's pixel format
    RawArray<Push> pushes;
    uint32_t collapsed;                         // number of open transform pushes that are collapsed
    PrPath* shape;
    PrPath* extra;
    PrPathPolyline outline;                         // a border's piece or a decoration
    RawArray<CGFloat> dashes;
    RawArray<PrDrawRampPiece> rampPieces;
    RawArray<PrColor> ramp;
    RawArray<CGFloat> components;          // a conic gradient's colors, straight
    RawArray<CGFloat> locations;           // where a conic gradient's colors are
};

CGAffineTransform affineOf(PrAffine2 t) {
    return CGAffineTransformMake(CGFloat(t.a), CGFloat(t.b), CGFloat(t.c), CGFloat(t.d), CGFloat(t.e), CGFloat(t.f));
}

CGAffineTransform affineOf(PrDAffine2 t) {
    return CGAffineTransformMake(CGFloat(t.a), CGFloat(t.b), CGFloat(t.c), CGFloat(t.d), CGFloat(t.e), CGFloat(t.f));
}

CGRect cgRect(PrRect r) {
    return CGRectMake(CGFloat(r.left), CGFloat(r.top), CGFloat(prRectWidth(r)), CGFloat(prRectHeight(r)));
}

// Returns the current space's transform to target pixels.
PrAffine2 toTarget(const Renderer& r) {
    CGAffineTransform t = CGAffineTransformConcat(CGContextGetCTM(r.context), CGAffineTransformInvert(r.target));
    return PrAffine2{float(t.a), float(t.b), float(t.c), float(t.d), float(t.tx), float(t.ty)};
}

// Concatenates onto the context's transform the transform that brings it to target pixels, and returns that transform.
// Concatenating its inverse goes back. A path already added keeps its place.
CGAffineTransform enterTarget(Renderer& r) {
    CGAffineTransform enter = CGAffineTransformConcat(r.target, CGAffineTransformInvert(CGContextGetCTM(r.context)));
    CGContextConcatCTM(r.context, enter);
    return enter;
}

void leaveTarget(Renderer& r, CGAffineTransform entered) {
    CGContextConcatCTM(r.context, CGAffineTransformInvert(entered));
}

// ---------------------------------------------------------------------------------------------------------------------
// Paths

// Appends the path to the context's current path.
void addPath(CGContextRef context, const PrPath* path) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t next = 0;
    auto at = [&](uint32_t i) { return CGPointMake(CGFloat(points[i].x), CGFloat(points[i].y)); };
    for (uint32_t i = 0; i < prPathVerbCount(path); ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move: {
                CGPoint p = at(next++);
                CGContextMoveToPoint(context, p.x, p.y);
                break;
            }
            case PrPathVerb_Line: {
                CGPoint p = at(next++);
                CGContextAddLineToPoint(context, p.x, p.y);
                break;
            }
            case PrPathVerb_Quad: {
                CGPoint c = at(next);
                CGPoint p = at(next + 1);
                next += 2;
                CGContextAddQuadCurveToPoint(context, c.x, c.y, p.x, p.y);
                break;
            }
            case PrPathVerb_Cubic: {
                CGPoint c1 = at(next);
                CGPoint c2 = at(next + 1);
                CGPoint p = at(next + 2);
                next += 3;
                CGContextAddCurveToPoint(context, c1.x, c1.y, c2.x, c2.y, p.x, p.y);
                break;
            }
            case PrPathVerb_Close:
                CGContextClosePath(context);
                break;
            default:
                break;
        }
    }
}

void addPolyline(CGContextRef context, const PrPathPolyline& polyline) {
    for (uint32_t c = 0; c < polyline.contourCount; ++c) {
        uint32_t start = polyline.contours[c];
        uint32_t end = prPathPolylineContourEnd(&polyline, c);
        if (end == start)
            continue;
        CGContextMoveToPoint(context, CGFloat(polyline.points[start].x), CGFloat(polyline.points[start].y));
        for (uint32_t i = start + 1; i < end; ++i)
            CGContextAddLineToPoint(context, CGFloat(polyline.points[i].x), CGFloat(polyline.points[i].y));
        CGContextClosePath(context);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Compositing

// Returns Quartz's mode for an operator, or for a blend mode if the operator is source-over. Quartz has no mode that
// does both.
CGBlendMode blendOf(PrDrawComposite composite, PrDrawBlendMode blend) {
    constexpr CGBlendMode kComposite[] = {
        kCGBlendModeNormal,          kCGBlendModeClear,           kCGBlendModeCopy,
        kCGBlendModeSourceIn,        kCGBlendModeSourceOut,       kCGBlendModeSourceAtop,
        kCGBlendModeDestinationOver, kCGBlendModeDestinationIn,   kCGBlendModeDestinationOut,
        kCGBlendModeDestinationAtop, kCGBlendModeXOR,             kCGBlendModePlusLighter,
        kCGBlendModePlusDarker,
    };
    constexpr CGBlendMode kBlend[] = {
        kCGBlendModeNormal,    kCGBlendModeMultiply,   kCGBlendModeScreen,
        kCGBlendModeOverlay,   kCGBlendModeDarken,     kCGBlendModeLighten,
        kCGBlendModeColorDodge, kCGBlendModeColorBurn, kCGBlendModeHardLight,
        kCGBlendModeSoftLight, kCGBlendModeDifference, kCGBlendModeExclusion,
        kCGBlendModeNormal,    // hard mix, which Quartz does not have
        kCGBlendModeHue,       kCGBlendModeSaturation, kCGBlendModeColor,
        kCGBlendModeLuminosity,
    };
    if (composite != PrDrawComposite_SourceOver && composite < PrDrawComposite_Count)
        return kComposite[composite];
    return blend < PrDrawBlendMode_Count ? kBlend[blend] : kCGBlendModeNormal;
}

// Begins and ends a painting command's compositing. Quartz applies the mode where the shape covers, which is where a
// command's operator applies.
void beginPaint(Renderer& r, const PrDrawCommand& c) {
    CGContextSaveGState(r.context);
    CGContextSetShouldAntialias(r.context, c.antialias);
    CGContextSetBlendMode(r.context, blendOf(c.composite, c.blendMode));
}

void endPaint(Renderer& r, const PrDrawCommand&) {
    CGContextRestoreGState(r.context);
}

void setFillColor(Renderer& r, PrColor color) {
    CGFloat components[4] = {CGFloat(color.r), CGFloat(color.g), CGFloat(color.b), CGFloat(prF32Clamp(color.a, 0, 1))};
    CGContextSetFillColorSpace(r.context, r.cg->srgb);
    CGContextSetFillColor(r.context, components);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

void releaseImageData(void* PR_NULLABLE info, const void*, size_t) {
    prDrawImageRelease(static_cast<const PrDrawImage*>(info));
}

// Returns a Quartz image over the image's pixels, which holds a reference to the image. It is created once and kept for
// as long as renders use it.
CGImageRef imageOf(CgBackend* cg, const PrDrawImage* image) {
    uint64_t id = prDrawImageId(image);
    uint32_t found = cg->imageIndex.find(id, [&](uint32_t i) { return cg->images.data[i].id; });
    if (found != kHashMissing) {
        cg->images.data[found].lastUsed = cg->render;
        return cg->images.data[found].image;
    }
    uint32_t width = prDrawImageExtent(image).x;
    uint32_t height = prDrawImageExtent(image).y;
    const PrPixelView pixels = prDrawImagePixels(image);
    size_t stride = pixels.stride;
    prDrawImageRetain(image);
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        const_cast<PrDrawImage*>(image), pixels.data, stride * height, releaseImageData);
    PrDrawAlpha alpha = prDrawImageAlpha(image);
    CGImageAlphaInfo info = kCGImageAlphaPremultipliedLast;
    if (alpha == PrDrawAlpha_Opaque)
        info = kCGImageAlphaNoneSkipLast;
    else if (alpha == PrDrawAlpha_Straight)
        info = kCGImageAlphaLast;
    CGImageRef made = nullptr;
    switch (prDrawImageFormat(image)) {
        case PrPixelFormat_Rgba8:
            made = CGImageCreate(width, height, 8, 32, stride, cg->srgb,
                CGBitmapInfo(info) | kCGBitmapByteOrder32Big, provider, nullptr, false, kCGRenderingIntentDefault);
            break;
        case PrPixelFormat_Bgra8: {
            CGImageAlphaInfo first = kCGImageAlphaPremultipliedFirst;
            if (alpha == PrDrawAlpha_Opaque)
                first = kCGImageAlphaNoneSkipFirst;
            else if (alpha == PrDrawAlpha_Straight)
                first = kCGImageAlphaFirst;
            made = CGImageCreate(width, height, 8, 32, stride, cg->srgb,
                CGBitmapInfo(first) | kCGBitmapByteOrder32Little, provider, nullptr, false, kCGRenderingIntentDefault);
            break;
        }
        case PrPixelFormat_RgbaF16:
            made = CGImageCreate(width, height, 16, 64, stride, cg->extended,
                CGBitmapInfo(info) | kCGBitmapByteOrder16Little | kCGBitmapFloatComponents,
                provider, nullptr, false, kCGRenderingIntentDefault);
            break;
        default:
            break;
    }
    CGDataProviderRelease(provider);
    cg->imageIndex.insert(id, cg->images.count);
    cg->images.push(CachedImage{id, made, cg->render});
    return made;
}

CGInterpolationQuality qualityOf(PrDrawSampling sampling) {
    switch (sampling & PrDrawSamplingFlag_Kind) {
        case PrDrawSampling_Nearest:
            return kCGInterpolationNone;
        case PrDrawSampling_Linear:
            return kCGInterpolationLow;
        default:
            return kCGInterpolationHigh;
    }
}

// Draws the image's `source` texels onto `destination` in the current space, reflected in either axis if requested.
void drawImageMapped(CGContextRef context, CGImageRef image, PrRect source, PrRect destination, bool reflectX,
        bool reflectY) {
    float sx = prRectWidth(destination) / prRectWidth(source);
    float sy = prRectHeight(destination) / prRectHeight(source);
    float width = float(CGImageGetWidth(image)) * sx;
    float height = float(CGImageGetHeight(image)) * sy;
    CGContextSaveGState(context);
    CGContextClipToRect(context, cgRect(destination));
    // Quartz draws an image upright with y pointing up. Flips it about the destination, and reflects it within the
    // destination if requested.
    CGContextTranslateCTM(context, CGFloat(destination.left), CGFloat(destination.top));
    if (reflectX) {
        CGContextTranslateCTM(context, CGFloat(prRectWidth(destination)), 0);
        CGContextScaleCTM(context, -1, 1);
    }
    if (!reflectY) {
        CGContextTranslateCTM(context, 0, CGFloat(prRectHeight(destination)));
        CGContextScaleCTM(context, 1, -1);
    }
    CGRect rect = CGRectMake(CGFloat(-source.left * sx), CGFloat((source.bottom - float(CGImageGetHeight(image))) * sy),
        CGFloat(width), CGFloat(height));
    CGContextDrawImage(context, rect, image);
    CGContextRestoreGState(context);
}

// ---------------------------------------------------------------------------------------------------------------------
// Brushes

// A shading's ramp: the pieces of the brush's colors, and how it extends.
struct Ramp {
    const PrDrawRampPiece* pieces;
    const PrColor* colors;
    PrDrawExtend extend;
};

void evaluateRamp(void* PR_NULLABLE info, const CGFloat* in, CGFloat* out) {
    const Ramp& ramp = *static_cast<const Ramp*>(info);
    float t = float(in[0]);
    if (!prDrawGradientExtend(ramp.extend, &t)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    const PrColor c = prDrawRampColor(&ramp.pieces[prDrawRampPieceAt(ramp.pieces, t, 0)], ramp.colors, t);
    // The ramp is premultiplied, and Quartz expects straight color.
    float alpha = c.a;
    out[0] = CGFloat(alpha > 0 ? c.r / alpha : 0);
    out[1] = CGFloat(alpha > 0 ? c.g / alpha : 0);
    out[2] = CGFloat(alpha > 0 ? c.b / alpha : 0);
    out[3] = CGFloat(alpha);
}

// Draws a shading of the ramp over the parameter's range [t0, t1].
void drawShading(Renderer& r, const Ramp& ramp, bool radial, CGPoint start, CGFloat startRadius,
        CGPoint end, CGFloat endRadius, float t0, float t1) {
    CGFloat domain[2] = {CGFloat(t0), CGFloat(t1)};
    CGFloat range[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    CGFunctionCallbacks callbacks{0, evaluateRamp, nullptr};
    CGFunctionRef function = CGFunctionCreate(const_cast<Ramp*>(&ramp), 1, domain, 4, range, &callbacks);
    bool extends = ramp.extend != PrDrawExtend_None;
    CGShadingRef shading;
    if (radial)
        shading = CGShadingCreateRadial(r.cg->srgb, start, startRadius, end, endRadius, function, extends, extends);
    else
        shading = CGShadingCreateAxial(r.cg->srgb, start, end, function, extends, extends);
    CGContextDrawShading(r.context, shading);
    CGShadingRelease(shading);
    CGFunctionRelease(function);
}

// Writes the corners of the clip's bounds in the current space.
void clipCorners(CGContextRef context, PrVec2 corners[4]) {
    CGRect box = CGContextGetClipBoundingBox(context);
    corners[0] = PrVec2{float(CGRectGetMinX(box)), float(CGRectGetMinY(box))};
    corners[1] = PrVec2{float(CGRectGetMaxX(box)), float(CGRectGetMinY(box))};
    corners[2] = PrVec2{float(CGRectGetMaxX(box)), float(CGRectGetMaxY(box))};
    corners[3] = PrVec2{float(CGRectGetMinX(box)), float(CGRectGetMaxY(box))};
}

void drawGradient(Renderer& r, const PrDrawBrush* brush, PrColor color) {
    r.rampPieces.resize(prDrawGradientRampMaxPieces(brush->stopCount));
    r.ramp.resize(prDrawGradientRampMaxColors(brush->stopCount, kRampResolution));
    r.rampPieces.resize(prDrawGradientRamp(brush, color, PrColorSpace_Srgb, kRampResolution, r.rampPieces.data,
        r.ramp.data));
    Ramp ramp{r.rampPieces.data, r.ramp.data, brush->extend};
    PrVec2 corners[4];
    clipCorners(r.context, corners);
    switch (brush->kind) {
        case PrDrawBrushKind_Linear: {
            // The axis spans the range of the parameter over the clip's corners.
            PrVec2 s = brush->linear.start;
            PrVec2 d{brush->linear.end.x - s.x, brush->linear.end.y - s.y};
            float length = d.x * d.x + d.y * d.y;
            if (!(length > 0))
                return;
            float t0 = kPrF32Max;
            float t1 = -kPrF32Max;
            for (const PrVec2& p : corners) {
                float t = ((p.x - s.x) * d.x + (p.y - s.y) * d.y) / length;
                t0 = prF32Min(t0, t);
                t1 = prF32Max(t1, t);
            }
            if (brush->extend == PrDrawExtend_Pad) {
                t0 = 0;
                t1 = 1;
            }
            CGPoint a = CGPointMake(CGFloat(s.x + d.x * t0), CGFloat(s.y + d.y * t0));
            CGPoint b = CGPointMake(CGFloat(s.x + d.x * t1), CGFloat(s.y + d.y * t1));
            drawShading(r, ramp, false, a, 0, b, 0, t0, t1);
            break;
        }
        case PrDrawBrushKind_Radial: {
            // Draws from the focal circle out to the circle that reaches the clip's farthest corner.
            const PrDrawRadialGradient& g = brush->radial;
            float t1 = 1;
            if (brush->extend != PrDrawExtend_Pad) {
                for (const PrVec2& p : corners) {
                    float t = 0;
                    if (prDrawGradientParameter(brush, p, &t))
                        t1 = prF32Max(t1, t);
                }
            }
            PrVec2 c1{g.start.x + (g.end.x - g.start.x) * t1, g.start.y + (g.end.y - g.start.y) * t1};
            float r1 = g.startRadius + (g.endRadius - g.startRadius) * t1;
            drawShading(r, ramp, true, CGPointMake(CGFloat(g.start.x), CGFloat(g.start.y)),
                CGFloat(g.startRadius), CGPointMake(CGFloat(c1.x), CGFloat(c1.y)), CGFloat(prF32Max(r1, 0)), 0, t1);
            break;
        }
        case PrDrawBrushKind_Diamond: {
            // A diamond is approximated by the ellipse through its corners.
            const PrDrawDiamondGradient& g = brush->diamond;
            if (!(g.radii.x > 0 && g.radii.y > 0))
                return;
            float t1 = 1;
            if (brush->extend != PrDrawExtend_Pad) {
                for (const PrVec2& p : corners) {
                    float dx = (p.x - g.center.x) / g.radii.x;
                    float dy = (p.y - g.center.y) / g.radii.y;
                    t1 = prF32Max(t1, prF32Sqrt(dx * dx + dy * dy));
                }
            }
            CGContextTranslateCTM(r.context, CGFloat(g.center.x), CGFloat(g.center.y));
            CGContextScaleCTM(r.context, CGFloat(g.radii.x), CGFloat(g.radii.y));
            drawShading(r, ramp, true, CGPointZero, 0, CGPointZero, CGFloat(t1), 0, t1);
            break;
        }
        case PrDrawBrushKind_Conic: {
            if (__builtin_available(macOS 14.0, *)) {
                // Each piece's colors are at their place in the piece, so that pieces that meet with different colors
                // are two colors at one location. A piece of one color is a color at each end.
                r.components.clear();
                r.locations.clear();
                for (uint32_t p = 1; p + 1 < r.rampPieces.count; ++p) {
                    const PrDrawRampPiece& piece = r.rampPieces.data[p];
                    uint32_t ends = piece.count == 1 ? 2 : piece.count;
                    for (uint32_t k = 0; k < ends; ++k) {
                        const PrColor& c = r.ramp.data[piece.first + (piece.count == 1 ? 0 : k)];
                        r.components.push(CGFloat(c.a > 0 ? c.r / c.a : 0));
                        r.components.push(CGFloat(c.a > 0 ? c.g / c.a : 0));
                        r.components.push(CGFloat(c.a > 0 ? c.b / c.a : 0));
                        r.components.push(CGFloat(c.a));
                        r.locations.push(CGFloat(piece.start) +
                            CGFloat(piece.end - piece.start) * CGFloat(k) / CGFloat(ends - 1));
                    }
                }
                CGGradientRef gradient = CGGradientCreateWithColorComponents(
                    r.cg->srgb, r.components.data, r.locations.data, r.locations.count);
                // Quartz turns clockwise if y points down, which matches the brush.
                const PrDrawConicGradient& g = brush->conic;
                CGContextDrawConicGradient(r.context, gradient, CGPointMake(CGFloat(g.center.x), CGFloat(g.center.y)),
                    CGFloat(g.angle));
                CGGradientRelease(gradient);
            }
            break;
        }
        default:
            break;
    }
}

// Draws the image brush's tiles over the clip, in brush space.
void drawTiles(Renderer& r, CGImageRef image, PrRect source, const PrDrawTiles& tiles, PrDrawSampling sampling) {
    PrRect tile = tiles.tile;
    float stepX = prRectWidth(tile) + tiles.spacing.x;
    float stepY = prRectHeight(tile) + tiles.spacing.y;
    if (!(stepX > 0 && stepY > 0))
        return;
    PrVec2 corners[4];
    clipCorners(r.context, corners);
    PrRect bounds{corners[0].x, corners[0].y, corners[2].x, corners[2].y};
    auto range = [](PrDrawExtend mode, float low, float high, float origin, float step, int32_t* from, int32_t* to) {
        if (mode == PrDrawExtend_Repeat || mode == PrDrawExtend_Reflect) {
            *from = int32_t(prF32Floor((low - origin) / step));
            *to = int32_t(prF32Floor((high - origin) / step));
        } else {
            *from = *to = 0;
        }
    };
    int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    range(tiles.x, bounds.left, bounds.right, tile.left, stepX, &x0, &x1);
    range(tiles.y, bounds.top, bounds.bottom, tile.top, stepY, &y0, &y1);
    if (double(x1 - x0 + 1) * double(y1 - y0 + 1) > kMaxTiles)
        return;
    CGContextSetInterpolationQuality(r.context, qualityOf(sampling));
    // The clip already provides the shape's edge. Tiles meet on whole pixels, with no seam between them.
    CGContextSetShouldAntialias(r.context, false);
    for (int32_t y = y0; y <= y1; ++y) {
        for (int32_t x = x0; x <= x1; ++x) {
            PrRect at{
                tile.left + float(x) * stepX, tile.top + float(y) * stepY,
                tile.right + float(x) * stepX, tile.bottom + float(y) * stepY,
            };
            bool reflectX = tiles.x == PrDrawExtend_Reflect && (x & 1) != 0;
            bool reflectY = tiles.y == PrDrawExtend_Reflect && (y & 1) != 0;
            drawImageMapped(r.context, image, source, at, reflectX, reflectY);
        }
    }
}

// Paints the brush times the color over the current clip, for a command whose fill geometry has `bounds` in the current
// space. A Target brush paints in target pixels. An image brush over a list paints nothing.
void paintBrush(Renderer& r, const PrDrawBrush* brush, PrColor color, PrRect bounds) {
    PrAffine2 fromSpace{};
    PrAffine2 toSpace{};
    if (!prDrawBrushFromTarget(brush, prAffine2Identity(), bounds, &fromSpace) || !prAffine2Invert(fromSpace, &toSpace))
        return;
    CGContextSaveGState(r.context);
    if (brush->space == PrDrawBrushSpace_Target)
        enterTarget(r);
    CGContextConcatCTM(r.context, affineOf(toSpace));
    switch (brush->kind) {
        case PrDrawBrushKind_Image: {
            const PrDrawSource& source = brush->image.source;
            if (source.kind == PrDrawSourceKind_Image) {
                CGContextSetAlpha(r.context, CGFloat(prF32Clamp(color.a, 0, 1)));
                drawTiles(r, imageOf(r.cg, source.image), source.rect, brush->image.tiles, brush->image.sampling);
            }
            break;
        }
        default:
            drawGradient(r, brush, color);
            break;
    }
    CGContextRestoreGState(r.context);
}

// Fills the context's current path with the command's paint: its color, or its brush under a clip to the path.
void fillCurrentPath(Renderer& r, PrPathFillRule rule, const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds) {
    if (brush == nullptr) {
        setFillColor(r, color);
        if (rule == PrPathFillRule_EvenOdd)
            CGContextEOFillPath(r.context);
        else
            CGContextFillPath(r.context);
        return;
    }
    CGContextSaveGState(r.context);
    if (rule == PrPathFillRule_EvenOdd)
        CGContextEOClip(r.context);
    else
        CGContextClip(r.context);
    paintBrush(r, brush, color, bounds);
    CGContextRestoreGState(r.context);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

CGLineJoin joinOf(PrPathJoin join) {
    switch (join) {
        case PrPathJoin_Round:
            return kCGLineJoinRound;
        case PrPathJoin_Bevel:
            return kCGLineJoinBevel;
        default:
            return kCGLineJoinMiter;
    }
}

CGLineCap capOf(PrPathCap cap) {
    switch (cap) {
        case PrPathCap_Round:
            return kCGLineCapRound;
        case PrPathCap_Square:
            return kCGLineCapSquare;
        default:
            return kCGLineCapButt;
    }
}

// Sets Quartz's stroke parameters to the stroke's parameters. A pattern with a negative length, or whose lengths sum to
// zero, strokes solid. An odd number of lengths is read twice over.
void setStroke(Renderer& r, const PrPathStroke& stroke) {
    CGContextSetLineWidth(r.context, CGFloat(stroke.width));
    CGContextSetMiterLimit(r.context, CGFloat(stroke.miterLimit));
    CGContextSetLineJoin(r.context, joinOf(stroke.join));
    CGContextSetLineCap(r.context, capOf(stroke.startCap));
    r.dashes.clear();
    float sum = 0;
    bool valid = true;
    for (uint32_t i = 0; i < stroke.dashCount; ++i) {
        valid = valid && stroke.dashes[i] >= 0;
        sum += stroke.dashes[i];
    }
    if (stroke.dashCount == 0 || !valid || !(sum > 0)) {
        CGContextSetLineDash(r.context, 0, nullptr, 0);
        return;
    }
    uint32_t count = stroke.dashCount % 2 != 0 ? stroke.dashCount * 2 : stroke.dashCount;
    for (uint32_t i = 0; i < count; ++i)
        r.dashes.push(CGFloat(stroke.dashes[i % stroke.dashCount]));
    CGContextSetLineDash(r.context, CGFloat(stroke.dashOffset), r.dashes.data, r.dashes.count);
}

// Fills or strokes `r.shape`, in the command's space, with its paint. A non-scaling stroke is measured in target
// pixels.
void drawShapePath(Renderer& r, const PrDrawCommand& c, PrRect bounds) {
    addPath(r.context, r.shape);
    if (c.stroke == nullptr) {
        fillCurrentPath(r, c.rule, c.brush, c.color, bounds);
        return;
    }
    if (!(c.stroke->shape.width > 0)) {
        CGContextBeginPath(r.context);
        return;
    }
    CGContextSaveGState(r.context);
    CGAffineTransform entered = CGAffineTransformIdentity;
    if (c.stroke->nonScaling)
        entered = enterTarget(r);
    setStroke(r, c.stroke->shape);
    if (c.brush == nullptr) {
        CGFloat components[4] = {
            CGFloat(c.color.r), CGFloat(c.color.g), CGFloat(c.color.b),
            CGFloat(prF32Clamp(prFloat16ToFloat(c.color.a), 0, 1)),
        };
        CGContextSetStrokeColorSpace(r.context, r.cg->srgb);
        CGContextSetStrokeColor(r.context, components);
        CGContextStrokePath(r.context);
    } else {
        CGContextReplacePathWithStrokedPath(r.context);
        if (c.stroke->nonScaling)
            leaveTarget(r, entered);
        fillCurrentPath(r, PrPathFillRule_NonZero, c.brush, c.color, bounds);
    }
    CGContextRestoreGState(r.context);
}

void drawShape(Renderer& r, const PrDrawCommand& c) {
    if (c.kind == PrDrawCommandKind_Line && c.stroke == nullptr)
        return;
    beginPaint(r, c);
    prPathClear(r.shape);
    prDrawShapePath(&c, r.shape);
    drawShapePath(r, c, prDrawFillBounds(&c));
    endPaint(r, c);
}

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

struct BorderDraw {
    Renderer* r;
    const PrDrawCommand* command;
};

void drawBorderPiece(void* PR_NULLABLE context, const PrDrawBorderPiece* piece,
        const PrPathPolyline* shape, const PrPathPolyline* PR_NULLABLE cut) {
    BorderDraw& d = *static_cast<BorderDraw*>(context);
    Renderer& r = *d.r;
    const PrDrawCommand& c = *d.command;
    const PrDrawBorderSide& side = c.border->sides[piece->side];
    CGContextSaveGState(r.context);
    if (cut != nullptr) {
        addPolyline(r.context, *cut);
        CGContextClip(r.context);
    }
    addPolyline(r.context, *shape);
    fillCurrentPath(r, piece->rule, side.brush, piece->color, c.border->shape.rect);
    CGContextRestoreGState(r.context);
}

void drawBorder(Renderer& r, const PrDrawCommand& c) {
    float scale = prAffine2MaxScale(toTarget(r));
    if (!(scale > 0))
        return;
    beginPaint(r, c);
    BorderDraw d{&r, &c};
    prDrawBorderLower(c.border, kTolerance / scale, drawBorderPiece, &d);
    endPaint(r, c);
}

// Casts the shadow of the context's current path, filled by `rule`, from outside the clip back onto it, offset by
// `offset` target pixels and blurred with Gaussian `sigma`. If `surround`, the path is first surrounded by a rectangle
// past the clip and the blur, so that its outside casts the shadow inward.
void castShadow(Renderer& r, PrColor color, PrVec2 offset, float sigma, PrPathFillRule rule, bool surround) {
    CGContextSaveGState(r.context);
    enterTarget(r);
    CGRect clip = CGContextGetClipBoundingBox(r.context);
    if (surround) {
        float reach = sigma * 4 + prF32Abs(offset.x) + prF32Abs(offset.y) + 64;
        CGContextAddRect(r.context, CGRectInset(clip, CGFloat(-reach), CGFloat(-reach)));
    }
    // Moves the path to just past the clip on the right, including the blur's extent. The path is copied out in target
    // pixels and added again under the move.
    CGRect path = CGContextGetPathBoundingBox(r.context);
    float away = float(CGRectGetMaxX(clip) - CGRectGetMinX(path)) + 8 * sigma + 16;
    CGPathRef shape = CGContextCopyPath(r.context);
    CGContextBeginPath(r.context);
    CGContextTranslateCTM(r.context, CGFloat(away), 0);
    CGContextAddPath(r.context, shape);
    CGPathRelease(shape);
    CGFloat components[4] = {CGFloat(color.r), CGFloat(color.g), CGFloat(color.b), CGFloat(prF32Clamp(color.a, 0, 1))};
    CGColorRef shadow = CGColorCreate(r.cg->srgb, components);
    // A shadow's offset is in the context's base space, where y points up. Its blur is twice the Gaussian's sigma.
    CGContextSetShadowWithColor(r.context, CGSizeMake(CGFloat(offset.x - away), CGFloat(-offset.y)), CGFloat(2 * sigma), shadow);
    CGColorRelease(shadow);
    setFillColor(r, PrColor{0, 0, 0, 1});
    if (rule == PrPathFillRule_EvenOdd)
        CGContextEOFillPath(r.context);
    else
        CGContextFillPath(r.context);
    CGContextRestoreGState(r.context);
}

void drawShadow(Renderer& r, const PrDrawCommand& c) {
    const PrDrawShadowKind kind =
        c.kind == PrDrawCommandKind_InnerShadow ? PrDrawShadowKind_Inner : PrDrawShadowKind_Drop;
    PrDrawShadowGeometry g = prDrawShadowGeometry(c.shadow, kind);
    float scale = c.shadow->nonScaling ? 1 : prAffine2MaxScale(toTarget(r));
    PrVec2 offset{0, 0};
    if (c.shadow->nonScaling)
        offset = c.shadow->offset;
    beginPaint(r, c);
    // The area where the shadow shows: outside the caster, or inside the opening.
    prPathClear(r.extra);
    prPathAddCornerRect(r.extra, c.shadow->shape);
    if (kind == PrDrawShadowKind_Inner) {
        addPath(r.context, r.extra);
        CGContextClip(r.context);
    } else {
        CGContextAddRect(r.context, CGContextGetClipBoundingBox(r.context));
        addPath(r.context, r.extra);
        CGContextEOClip(r.context);
    }
    prPathClear(r.shape);
    prPathAddCornerRect(r.shape, g.shape);
    addPath(r.context, r.shape);
    castShadow(r, c.color, offset, g.sigma * scale,
        kind == PrDrawShadowKind_Inner ? PrPathFillRule_EvenOdd : PrPathFillRule_NonZero,
        kind == PrDrawShadowKind_Inner);
    endPaint(r, c);
}

void drawDecoration(Renderer& r, const PrDrawCommand& c) {
    float scale = prAffine2MaxScale(toTarget(r));
    if (!(scale > 0))
        return;
    beginPaint(r, c);
    prPathPolylineClear(&r.outline);
    prDrawDecorationOutline(c.decoration, kTolerance / scale, &r.outline);
    addPolyline(r.context, r.outline);
    fillCurrentPath(r, PrPathFillRule_NonZero, c.brush, c.color, prDrawFillBounds(&c));
    endPaint(r, c);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Draws rectangles of the command's source, measured from the corner of its rect, with the color's alpha. A list source
// draws nothing.
void drawImageRects(Renderer& r, const PrDrawCommand& c, const PrDrawImageRect* rects, uint32_t count) {
    const PrDrawSource& source = c.nineSlice.source;
    if (source.kind != PrDrawSourceKind_Image)
        return;
    CGImageRef image = imageOf(r.cg, source.image);
    if (image == nullptr)
        return;
    beginPaint(r, c);
    CGContextSetAlpha(r.context, CGFloat(prF32Clamp(prFloat16ToFloat(c.color.a), 0, 1)));
    CGContextSetInterpolationQuality(r.context, qualityOf(c.sampling));
    for (uint32_t i = 0; i < count; ++i) {
        const PrRect& s = rects[i].source;
        PrRect texels{
            source.rect.left + s.left, source.rect.top + s.top, source.rect.left + s.right,
            source.rect.top + s.bottom,
        };
        drawImageMapped(r.context, image, texels, rects[i].destination, false, false);
    }
    endPaint(r, c);
}

void drawNineSlice(Renderer& r, const PrDrawCommand& c) {
    const PrRect& area = c.nineSlice.source.rect;
    float width = prRectWidth(area);
    float height = prRectHeight(area);
    uint32_t count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{width, height}, c.nineSlice.destination, nullptr, 0);
    RawArray<PrDrawImageRect> pieces;
    pieces.reserve(count);
    pieces.count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{width, height}, c.nineSlice.destination, pieces.data, count);
    drawImageRects(r, c, pieces.data, pieces.count);
    pieces.reset();
}

// Replaces the target's pixels at the device point with the image's, ignoring all state.
void writePixels(Renderer& r, const PrDrawCommand& c) {
    if (r.pixels == nullptr)
        return;
    CGContextFlush(r.context);
    prDrawImageWritePixels(c.writePixels.image, *r.pixels, r.format, c.writePixels.origin);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

void runCommands(Renderer& r, const PrDrawCommand* commands, uint32_t count);

// Plays a color glyph's list at the glyph.
void drawColorGlyph(Renderer& r, const PrDrawCommand& c, PrDrawList* drawn, uint32_t index) {
    PrDrawGroup group{};
    PrDrawCommand commands[kPrDrawGlyphMaxCommands];
    runCommands(r, commands, prDrawGlyphListCommands(&c, index, drawn, &group, commands));
}

// Returns the font's face at its design point as a Quartz font created from its own bytes. It is created once and kept
// for as long as renders use it. Returns null for a face Quartz cannot read, which is any face except a collection's
// first.
CGFontRef PR_NULLABLE graphicsFontOf(CgBackend* cg, const PrFont* font) {
    uint64_t id = prFontId(font);
    for (uint32_t i = 0; i < cg->fonts.count; ++i) {
        if (cg->fonts.data[i].id == id) {
            cg->fonts.data[i].lastUsed = cg->render;
            return cg->fonts.data[i].font;
        }
    }
    CGFontRef made = nullptr;
    if (prFontIndex(font) == 0) {
        PrByteView bytes = prFontData(font);
        CFDataRef data = CFDataCreate(nullptr, bytes.data, CFIndex(bytes.size));
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        made = CGFontCreateWithDataProvider(provider);
        CGDataProviderRelease(provider);
        CFRelease(data);
    }
    // Quartz identifies a variation's axes by name instead of by tag. Each axis is matched to the font's axis with the
    // same range. A font at its default instance is left unchanged, because Quartz's copy of a font at its defaults
    // draws some glyphs as nothing.
    bool varied = false;
    for (uint32_t k = 0; k < prFontAxisCount(font); ++k)
        varied = varied || prFontAxisValue(font, k) != prFontAxis(font, k).defaultValue;
    CFArrayRef axes = made != nullptr && varied ? CGFontCopyVariationAxes(made) : nullptr;
    if (axes != nullptr) {
        CFMutableDictionaryRef values = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        for (CFIndex i = 0; i < CFArrayGetCount(axes); ++i) {
            CFDictionaryRef axis = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(axes, i));
            auto numberOf = [&](CFStringRef key) {
                double v = 0;
                CFNumberRef n = static_cast<CFNumberRef>(CFDictionaryGetValue(axis, key));
                if (n != nullptr)
                    CFNumberGetValue(n, kCFNumberDoubleType, &v);
                return float(v);
            };
            float minimum = numberOf(kCGFontVariationAxisMinValue);
            float maximum = numberOf(kCGFontVariationAxisMaxValue);
            float fallback = numberOf(kCGFontVariationAxisDefaultValue);
            const void* name = CFDictionaryGetValue(axis, kCGFontVariationAxisName);
            for (uint32_t k = 0; k < prFontAxisCount(font) && name != nullptr; ++k) {
                PrFontAxis a = prFontAxis(font, k);
                if (prF32Abs(a.min - minimum) < 0.01f && prF32Abs(a.max - maximum) < 0.01f &&
                        prF32Abs(a.defaultValue - fallback) < 0.01f) {
                    double value = double(prFontAxisValue(font, k));
                    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberDoubleType, &value);
                    CFDictionarySetValue(values, name, number);
                    CFRelease(number);
                }
            }
        }
        CGFontRef copy = CGFontCreateCopyWithVariations(made, values);
        if (copy != nullptr) {
            CGFontRelease(made);
            made = copy;
        }
        CFRelease(values);
        CFRelease(axes);
    }
    cg->fonts.push(CachedFont{id, made, cg->render});
    return made;
}

// Returns the CoreText font for a Quartz font at the size, which is created once per render.
CTFontRef sizedFontOf(CgBackend* cg, uint64_t id, CGFontRef graphics, float size) {
    for (uint32_t i = 0; i < cg->sized.count; ++i) {
        if (cg->sized.data[i].id == id && cg->sized.data[i].size == size)
            return cg->sized.data[i].font;
    }
    CTFontRef font = CTFontCreateWithGraphicsFont(graphics, CGFloat(size), nullptr, nullptr);
    cg->sized.push(SizedFont{id, size, font});
    return font;
}

// Draws a glyph with CoreText under its placement, with y up about its origin.
void showGlyph(Renderer& r, const PrFontGlyphRun* run, uint32_t index, CTFontRef font) {
    PrAffine2 t{1, 0, -prFontOblique(run->font), 1, 0, 0};
    if (run->transforms != nullptr)
        t = prAffine2Concat(t, run->transforms[index]);
    PrVec2 at = run->glyphs[index].position;
    t = prAffine2Concat(t, prAffine2Translate(at));
    CGContextSaveGState(r.context);
    CGContextConcatCTM(r.context, CGAffineTransformScale(affineOf(t), 1, -1));
    CGGlyph glyph = CGGlyph(run->glyphs[index].id);
    CGPoint origin = CGPointZero;
    CTFontDrawGlyphs(font, &glyph, &origin, 1, r.context);
    CGContextRestoreGState(r.context);
}

// Fills each glyph separately. A color glyph is drawn with its layers, because CoreText does not draw COLR version 1.
// The other glyphs are drawn by CoreText if it can draw them, and otherwise as outlines. Outlines are used under a
// brush, or with a negative embolden.
void drawGlyphs(Renderer& r, const PrDrawCommand& c) {
    const PrFontGlyphRun* run = c.glyphs;
    if (c.stroke != nullptr) {
        drawShape(r, c);
        return;
    }
    const PrFont* font = run->font;
    CTFontRef native = nullptr;
    if (c.brush == nullptr && !(prFontEmbolden(font) < 0)) {
        if (CGFontRef graphics = graphicsFontOf(r.cg, font))
            native = sizedFontOf(r.cg, prFontId(font), graphics, run->size);
    }
    PrRect bounds = prDrawFillBounds(&c);
    PrColor foreground = c.color;
    foreground.a = 1;
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        uint16_t glyph = run->glyphs[i].id;
        float pixelsPerEm = prDrawGlyphPixelsPerEm(run, i, toTarget(r));
        PrDrawList* drawn = prDrawGlyphCacheList(r.cg->glyphs, font, glyph, pixelsPerEm, &run->palette, foreground);
        if (drawn != nullptr) {
            drawColorGlyph(r, c, drawn, i);
            continue;
        }
        if (native != nullptr) {
            beginPaint(r, c);
            CGContextSetShouldSmoothFonts(r.context, r.cg->options.fontSmoothing);
            CGContextSetAllowsFontSmoothing(r.context, r.cg->options.fontSmoothing);
            CGContextSetShouldSubpixelPositionFonts(r.context, true);
            CGContextSetShouldSubpixelQuantizeFonts(r.context, false);
            setFillColor(r, c.color);
            float embolden = prFontEmbolden(font);
            if (embolden > 0) {
                // Strokes with the fill color, at twice the growth.
                CGFloat components[4] = {
                    CGFloat(c.color.r), CGFloat(c.color.g), CGFloat(c.color.b),
                    CGFloat(prF32Clamp(prFloat16ToFloat(c.color.a), 0, 1)),
                };
                CGContextSetStrokeColorSpace(r.context, r.cg->srgb);
                CGContextSetStrokeColor(r.context, components);
                CGContextSetLineWidth(r.context, CGFloat(2 * embolden * run->size));
                CGContextSetLineJoin(r.context, kCGLineJoinRound);
                CGContextSetTextDrawingMode(r.context, kCGTextFillStroke);
            }
            showGlyph(r, run, i, native);
            endPaint(r, c);
            continue;
        }
        PrAffine2 x = toTarget(r);
        prPathClear(r.shape);
        if (!prDrawGlyphPath(run, i, &x, r.shape))
            continue;
        beginPaint(r, c);
        addPath(r.context, r.shape);
        fillCurrentPath(r, PrPathFillRule_NonZero, c.brush, c.color, bounds);
        endPaint(r, c);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Groups

// Clips to the group's mask: its brush times its color over `bounds` of the current space. Quartz draws the mask into
// an alpha-only layer over the clip in target pixels. An inverted mask is the brush cut out of full coverage. A
// luminance mask is read as alpha.
void clipToMask(Renderer& r, const PrDrawMask* mask, PrRect bounds) {
    CGAffineTransform space = CGContextGetCTM(r.context);
    CGAffineTransform entered = enterTarget(r);
    CGAffineTransform toPixels = CGAffineTransformConcat(space, CGAffineTransformInvert(r.target));
    CGRect box = CGRectIntegral(CGContextGetClipBoundingBox(r.context));
    if (!prRectIsEmpty(bounds))
        box = CGRectIntersection(box, CGRectIntegral(CGRectApplyAffineTransform(cgRect(bounds), toPixels)));
    size_t width = CGRectIsNull(box) ? 0 : size_t(box.size.width);
    size_t height = CGRectIsNull(box) ? 0 : size_t(box.size.height);
    if (width == 0 || height == 0) {
        CGContextClipToRect(r.context, CGRectZero);
        leaveTarget(r, entered);
        return;
    }
    uint8_t* values = static_cast<uint8_t*>(prMemoryAllocZeroed(width * height));
    CGContextRef layer = CGBitmapContextCreate(values, width, height, 8, width, nullptr,
        CGBitmapInfo(kCGImageAlphaOnly));
    // Maps target pixels, with y down, over the box.
    CGContextTranslateCTM(layer, 0, CGFloat(height));
    CGContextScaleCTM(layer, 1, -1);
    CGContextTranslateCTM(layer, -box.origin.x, -box.origin.y);
    if (mask->invert) {
        CGContextFillRect(layer, box);
        CGContextSetBlendMode(layer, kCGBlendModeDestinationOut);
    }
    CGContextRef context = r.context;
    CGAffineTransform target = r.target;
    r.context = layer;
    r.target = CGContextGetCTM(layer);
    CGContextConcatCTM(layer, toPixels);
    paintBrush(r, &mask->brush, PrColor{1, 1, 1, mask->brush.opacity}, bounds);
    r.context = context;
    r.target = target;
    CGContextRelease(layer);
    // The layer's alpha, read as a gray image, clips as alpha. It is drawn upright, because y points up.
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, values, width * height,
        [](void*, const void* data, size_t) { prMemoryFree(const_cast<void*>(data)); });
    CGImageRef image = CGImageCreate(width, height, 8, 8, width, r.cg->gray,
        CGBitmapInfo(kCGImageAlphaNone), provider, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGContextTranslateCTM(r.context, box.origin.x, box.origin.y + box.size.height);
    CGContextScaleCTM(r.context, 1, -1);
    CGContextClipToMask(r.context, CGRectMake(0, 0, CGFloat(width), CGFloat(height)), image);
    CGContextScaleCTM(r.context, 1, -1);
    CGContextTranslateCTM(r.context, -box.origin.x, -(box.origin.y + box.size.height));
    CGImageRelease(image);
    leaveTarget(r, entered);
}

// Clips to where a projected group's plane lands, and concatenates onto the context's transform the projection's
// tangent at the center of the plane's visible part, so that the group's layer holds its plane under that tangent.
// Returns false if the group draws nothing.
bool enterPlane(Renderer& r, const PrDrawGroup* g) {
    PrAffine2 space = toTarget(r);
    enterTarget(r);
    CGRect clip = CGContextGetClipBoundingBox(r.context);
    PrRect visible;
    if (CGRectIsNull(clip))
        visible = PrRect{0, 0, 0, 0};
    else
        visible = PrRect{
            float(CGRectGetMinX(clip)), float(CGRectGetMinY(clip)), float(CGRectGetMaxX(clip)),
            float(CGRectGetMaxY(clip)),
        };
    PrDrawProjection p{};
    if (!prDrawProjectionPlace(g->projection, space, g->bounds, visible, float(kPrDrawRenderMaxSize), &p))
        return false;
    prPathPolylineClear(&r.outline);
    prDrawProjectionQuad(&p, prAffine2MapRect(prAffine2Scale(PrVec2{p.scale, p.scale}), p.area), &r.outline);
    addPolyline(r.context, r.outline);
    CGContextClip(r.context);
    CGContextConcatCTM(r.context, affineOf(prDrawProjectionTangent(&p)));
    return true;
}

void pushGroup(Renderer& r, const PrDrawCommand& c) {
    const PrDrawGroup* g = c.group;
    Push push{PrDrawCommandKind_PushGroup, true, prDrawGroupHasEffect(g), false};
    CGContextSaveGState(r.context);
    if (g->projection != nullptr && !enterPlane(r, g)) {
        CGContextClipToRect(r.context, CGRectZero);
        push.layered = false;
    }
    if (push.layered) {
        CGContextSetAlpha(r.context, CGFloat(prF32Clamp(g->opacity, 0, 1)));
        CGContextSetBlendMode(r.context, blendOf(g->composite, g->blendMode));
        // A filter that is one drop shadow becomes Quartz's shadow on the layer.
        if (g->filter != nullptr && prDrawFilterNodeCount(g->filter) == 1 &&
                prDrawFilterNodes(g->filter)[0].kind == PrDrawFilterKind_DropShadow) {
            const PrDrawFilterNode& node = prDrawFilterNodes(g->filter)[0];
            PrAffine2 near = toTarget(r);
            PrVec2 offset{
                near.a * node.shadow.offset.x + near.c * node.shadow.offset.y,
                near.b * node.shadow.offset.x + near.d * node.shadow.offset.y,
            };
            float sigma = prF32Max(node.shadow.sigma.x, node.shadow.sigma.y) * prAffine2MaxScale(near);
            PrColor shadow = node.shadow.color;
            CGFloat components[4] = {
                CGFloat(shadow.r), CGFloat(shadow.g), CGFloat(shadow.b),
                CGFloat(prF32Clamp(shadow.a, 0, 1)),
            };
            CGColorRef color = CGColorCreate(r.cg->srgb, components);
            CGContextSetShadowWithColor(r.context, CGSizeMake(CGFloat(offset.x), CGFloat(-offset.y)),
                CGFloat(2 * sigma), color);
            CGColorRelease(color);
        }
        bool bounded = !prRectIsEmpty(g->bounds) && !prDrawCompositeIsUnbounded(g->composite);
        if (g->mask != nullptr) {
            PrRect bounds = g->bounds;
            if (prRectIsEmpty(bounds)) {
                CGRect clip = CGContextGetClipBoundingBox(r.context);
                bounds = PrRect{
                    float(CGRectGetMinX(clip)), float(CGRectGetMinY(clip)), float(CGRectGetMaxX(clip)),
                    float(CGRectGetMaxY(clip)),
                };
            }
            clipToMask(r, g->mask, bounds);
        }
        CGRect box = CGContextGetClipBoundingBox(r.context);
        if (bounded)
            box = CGRectIntersection(box, cgRect(g->bounds));
        CGContextBeginTransparencyLayerWithRect(r.context, CGRectIsNull(box) ? CGRectZero : box, nullptr);
    }
    r.pushes.push(push);
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

void runCommands(Renderer& r, const PrDrawCommand* commands, uint32_t count);

void initRenderer(Renderer& r, CgBackend* cg, CGContextRef context, const PrPixels* PR_NULLABLE pixels, PrPixelFormat format) {
    r.cg = cg;
    r.context = context;
    r.pixels = pixels;
    r.format = format;
    r.target = CGContextGetCTM(context);
    r.shape = prPathCreate();
    r.extra = prPathCreate();
}

void finishRenderer(Renderer& r) {
    r.pushes.reset();
    r.rampPieces.reset();
    r.ramp.reset();
    r.components.reset();
    r.locations.reset();
    r.dashes.reset();
    prPathDestroy(r.shape);
    prPathDestroy(r.extra);
    prPathPolylineReset(&r.outline);
}

// Creates a bitmap context over the pixels, with y down from its top left, matching the list's device space.
CGContextRef PR_NULLABLE contextOver(CgBackend* cg, PrPixels pixels, PrPixelFormat format) {
    CGBitmapInfo info;
    if (format == PrPixelFormat_Bgra8)
        info = CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little;
    else
        info = CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
    CGContextRef context =
        CGBitmapContextCreate(pixels.data, pixels.extent.x, pixels.extent.y, 8, pixels.stride, cg->srgb, info);
    if (context == nullptr)
        return nullptr;
    CGContextTranslateCTM(context, 0, CGFloat(pixels.extent.y));
    CGContextScaleCTM(context, 1, -1);
    return context;
}

void pushClip(Renderer& r, const PrDrawCommand& c) {
    CGContextSaveGState(r.context);
    CGContextSetShouldAntialias(r.context, c.antialias);
    if (c.kind == PrDrawCommandKind_PushClipTargetRect) {
        CGAffineTransform entered = enterTarget(r);
        CGContextClipToRect(r.context, cgRect(c.rect));
        leaveTarget(r, entered);
    } else {
        prPathClear(r.shape);
        prDrawShapePath(&c, r.shape);
        addPath(r.context, r.shape);
        if (prPathVerbCount(r.shape) == 0)
            CGContextClipToRect(r.context, CGRectZero);
        else if (c.rule == PrPathFillRule_EvenOdd)
            CGContextEOClip(r.context);
        else
            CGContextClip(r.context);
    }
    r.pushes.push(Push{PrDrawCommandKind_PushClipRect, true, false, false});
}

void pop(Renderer& r) {
    Push push = r.pushes.data[--r.pushes.count];
    if (push.collapsed)
        --r.collapsed;
    if (push.layered)
        CGContextEndTransparencyLayer(r.context);
    if (push.saved)
        CGContextRestoreGState(r.context);
}

void clearTo(Renderer& r, PrColor color) {
    CGContextSaveGState(r.context);
    CGContextSetBlendMode(r.context, kCGBlendModeCopy);
    enterTarget(r);
    setFillColor(r, color);
    CGContextFillRect(r.context, CGContextGetClipBoundingBox(r.context));
    CGContextRestoreGState(r.context);
}

void runCommands(Renderer& r, const PrDrawCommand* commands, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = commands[i];
        if (r.collapsed != 0 && prDrawCommandSkipsCollapsed(&c)) {
            // A push still opens a scope that its pop closes.
            if (c.kind >= PrDrawCommandKind_PushClipRect && c.kind <= PrDrawCommandKind_PushClipPath) {
                CGContextSaveGState(r.context);
                CGContextClipToRect(r.context, CGRectZero);
                r.pushes.push(Push{c.kind, true, false, false});
            } else if (c.kind == PrDrawCommandKind_PushGroup) {
                r.pushes.push(Push{c.kind, false, false, false});
            }
            continue;
        }
        switch (c.kind) {
            case PrDrawCommandKind_Clear:
                clearTo(r, c.color);
                break;
            case PrDrawCommandKind_Border:
                drawBorder(r, c);
                break;
            case PrDrawCommandKind_DropShadow:
            case PrDrawCommandKind_InnerShadow:
                drawShadow(r, c);
                break;
            case PrDrawCommandKind_Decoration:
                drawDecoration(r, c);
                break;
            case PrDrawCommandKind_NineSlice:
                drawNineSlice(r, c);
                break;
            case PrDrawCommandKind_WritePixels:
                writePixels(r, c);
                break;
            case PrDrawCommandKind_Glyphs:
                drawGlyphs(r, c);
                break;
            case PrDrawCommandKind_Embed: {
                // The offset plays as a push of its translation.
                PrDrawCommand moves[2]{};
                moves[0].kind = PrDrawCommandKind_PushTransformAffine2;
                moves[0].affine = prDAffine2Translate(c.embed.offset.x, c.embed.offset.y);
                moves[1].kind = PrDrawCommandKind_PopTransform;
                runCommands(r, &moves[0], 1);
                runCommands(r, prDrawListCommands(c.embed.list), prDrawListCommandCount(c.embed.list));
                runCommands(r, &moves[1], 1);
                break;
            }
            case PrDrawCommandKind_PushTransformAffine2: {
                // Nothing draws under a collapsed transform, so Quartz is not given it.
                PrDAffine2 pushed = c.affine;
                Push push{c.kind, true, false, false};
                CGContextSaveGState(r.context);
                push.collapsed = r.collapsed != 0 ||
                    prDrawAffine2Collapses(prAffine2Concat(prAffine2FromDAffine2(pushed), toTarget(r)));
                if (push.collapsed)
                    ++r.collapsed;
                else
                    CGContextConcatCTM(r.context, affineOf(pushed));
                r.pushes.push(push);
                break;
            }
            case PrDrawCommandKind_PushClipRect:
            case PrDrawCommandKind_PushClipTargetRect:
            case PrDrawCommandKind_PushClipCornerRect:
            case PrDrawCommandKind_PushClipPath:
                pushClip(r, c);
                break;
            case PrDrawCommandKind_PushGroup:
                pushGroup(r, c);
                break;
            case PrDrawCommandKind_PopTransform:
            case PrDrawCommandKind_PopClip:
            case PrDrawCommandKind_PopGroup:
                pop(r);
                break;
            default:
                drawShape(r, c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The backend

void destroyBackend(PrDrawRenderer* renderer) {
    CgBackend* cg = static_cast<CgBackend*>(renderer);
    for (uint32_t i = 0; i < cg->images.count; ++i)
        CGImageRelease(cg->images.data[i].image);
    prDrawGlyphCacheDestroy(cg->glyphs);
    cg->images.reset();
    cg->imageIndex.reset();
    for (uint32_t i = 0; i < cg->fonts.count; ++i)
        CGFontRelease(cg->fonts.data[i].font);
    cg->fonts.reset();
    cg->sized.reset();
    CGColorSpaceRelease(cg->srgb);
    CGColorSpaceRelease(cg->extended);
    CGColorSpaceRelease(cg->gray);
    if (cg->ownsDevice)
        prGpuDeviceDestroy(cg->device);
    prMemoryFree(cg);
}

void destroyFrame(PrDrawRenderer*, PrDrawFrame* frame) {
    drawMemoryFrameDestroy(static_cast<DrawMemoryFrame*>(frame));
}

// Drops the cached data that the render did not use.
void endRender(CgBackend* cg) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < cg->images.count; ++i) {
        if (cg->images.data[i].lastUsed == cg->render)
            cg->images.data[kept++] = cg->images.data[i];
        else
            CGImageRelease(cg->images.data[i].image);
    }
    cg->images.count = kept;
    cg->imageIndex.rebuild(kept, [&](uint32_t i) { return cg->images.data[i].id; });
    prDrawGlyphCacheEnd(cg->glyphs);
    kept = 0;
    for (uint32_t i = 0; i < cg->fonts.count; ++i) {
        if (cg->fonts.data[i].lastUsed == cg->render)
            cg->fonts.data[kept++] = cg->fonts.data[i];
        else
            CGFontRelease(cg->fonts.data[i].font);
    }
    cg->fonts.count = kept;
    for (uint32_t i = 0; i < cg->sized.count; ++i)
        CFRelease(cg->sized.data[i].font);
    cg->sized.clear();
}

void renderPixels(PrDrawRenderer* renderer, PrPixels pixels, PrPixelFormat format, const PrDrawList* list) {
    CgBackend* cg = static_cast<CgBackend*>(renderer);
    CGContextRef context = contextOver(cg, pixels, format);
    if (context == nullptr)
        return;
    ++cg->render;
    prDrawGlyphCacheBegin(cg->glyphs);
    Renderer r{};
    initRenderer(r, cg, context, &pixels, format);
    runCommands(r, prDrawListCommands(list), prDrawListCommandCount(list));
    finishRenderer(r);
    CGContextFlush(context);
    CGContextRelease(context);
    endRender(cg);
}

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    return drawMemoryFrameCreate(renderer, renderPixels, false);
}

// Keeps the list, which renders when the frame's pass runs.
PrStatus prepare(PrDrawRenderer*, PrDrawFrame* frame, const PrDrawList* list, const PrDrawTarget*) {
    drawMemoryFramePrepare(static_cast<DrawMemoryFrame*>(frame), list);
    return PrStatus_Ok;
}

void renderFrame(PrDrawRenderer*, PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin) {
    drawMemoryFrameRender(static_cast<DrawMemoryFrame*>(frame), pass, origin);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawCoreGraphicsOptions prDrawCoreGraphicsOptionsDefault(void) {
    return PrDrawCoreGraphicsOptions{.fontSmoothing = true};
}

PrDrawRenderer* PR_NULLABLE prDrawCoreGraphicsCreate(const PrDrawCoreGraphicsOptions* PR_NULLABLE options) {
    PrDrawCoreGraphicsOptions o = options != nullptr ? *options : prDrawCoreGraphicsOptionsDefault();
    bool owned = false;
    PrGpuDevice* device = drawMemoryDevice(o.device, &owned);
    if (device == nullptr)
        return nullptr;
    CgBackend* cg = allocZeroed<CgBackend>();
    cg->device = device;
    cg->ownsDevice = owned;
    cg->options = o;
    cg->glyphs = prDrawGlyphCacheCreate(cg->options.glyphDecoder);
    cg->destroy = destroyBackend;
    cg->frameCreate = createFrame;
    cg->frameDestroy = destroyFrame;
    cg->framePrepare = prepare;
    cg->frameRender = renderFrame;
    cg->srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    cg->extended = CGColorSpaceCreateWithName(kCGColorSpaceExtendedSRGB);
    cg->gray = CGColorSpaceCreateDeviceGray();
    return cg;
}
