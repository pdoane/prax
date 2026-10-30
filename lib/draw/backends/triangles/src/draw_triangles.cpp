// The triangles draw backend, which turns a list into textured, colored triangles.
//
// A build walks the list in the same way as every backend. Shapes, strokes, borders and decorations become paths in
// target pixels, which the tessellator fills with a one-pixel fringe for their coverage. The paint is evaluated at each
// vertex: a gradient's color at the vertex, or an image brush's texture coordinate. Clips become scissor rectangles. A
// clip that is not an axis-aligned rectangle uses its bounds. A group's opacity multiplies what it draws. A shadow is a
// band a little wider than its blur, whose coverage falls off linearly across it.
//
// Glyphs drawn without rotation or skew at modest sizes come from the glyph atlas, which has a texture for each of its
// pages. Other glyphs are filled as paths. The first atlas texture holds a white block that untextured triangles
// sample, so that they draw in the same draws as text. An image is uploaded as its own texture the first time a build
// draws it.

#include "pr/draw_triangles.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/draw_commands.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_shared_atlas.h"
#include "pr/draw_transform.h"
#include "pr/float.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/path_measure.h"
#include "pr/path_polyline.h"
#include "pr/path_tessellator.h"
#include "pr/vec.h"

#include "draw/backends/triangles/src/draw_triangles_device.h"
#include "draw/backends/triangles/src/draw_triangles_frame.h"

namespace pr_impl {

namespace {

// The largest distance of a flattened curve from the curve, in target pixels.
constexpr float kTolerance = 0.25f;

// The maximum number of tiles that a list brush plays.
constexpr uint32_t kListMaxTiles = 4096;

// The largest distance of a flattened curve from the curve in a clip's outline, in target pixels. The outline cuts
// every triangle drawn within the clip.
constexpr float kClipTolerance = 0.1f;

// The width of the band that antialiases an edge, in target pixels.
constexpr float kFringe = 1;

// The number of consecutive builds in which a texture goes unused before it is destroyed.
constexpr uint64_t kIdleBuilds = 8;

// The white block at the corner of the first atlas texture, in texels.
constexpr uint32_t kWhiteSize = 4;

// The number of colors that a gradient's parameter selects from.
constexpr uint32_t kRampSize = 256;

// The minimum number of wedges that a conic gradient's pieces divide a turn into.
constexpr uint32_t kConicWedges = 36;

// The ratio of a shadow's band to the sigma of its blur. Across a straight edge, a band this wide spreads a little less
// coverage past the edge than the blur does, and the band's rounder corners make up the difference.
constexpr float kShadowBand = 2.8f;

// ---------------------------------------------------------------------------------------------------------------------
// State

struct CachedImage {
    uint64_t id;
    uint32_t texture;
    uint64_t lastUsed;
};

// A gradient's colors for a color, the points where the colors turn, and the rays that cut a conic gradient.
struct CachedRamp {
    const PrDrawBrush* brush;
    PrColor color;
    uint32_t breakCount;                // points where the ramp's linear pieces meet, ascending from 0 to 1
    float breaks[kRampSize];
    uint32_t rayCount;                  // the breaks and kConicWedges steps, ascending from 0 to 1
    float rays[kRampSize + kConicWedges + 1];
    uint32_t colors[kRampSize];         // straight
};

// The state that builds share: the textures, and scratch memory.
struct Triangles {
    uint32_t atlasSize;
    PrDrawAtlas* atlas;
    Array<uint32_t> pageTextures;       // the texture of each atlas page
    uint32_t pagesCreated;              // number of atlas pages whose textures the host has
    PrDrawGlyphCache* lists;
    PrPathTessellator* tessellator;
    PrPathPolyline polyline;
    uint64_t build;
    uint32_t nextTexture;
    Array<CachedImage> images;
    KeyIndex imageIndex;                // `images`, by id
    Array<CachedRamp*> ramps;           // owned; the first `rampCount` belong to this build
    uint32_t rampCount;                 // valid until the build ends, while the list's brushes stay put
    HashIndex rampIndex;                // this build's ramps, by the inputs that decide their colors
    PrPath* shape;
    PrPath* mapped;
    PrPath* region;
    PrPathPolyline fill;                    // a stroke's outlines (prDrawLowerStroke)
    PrPathPolyline aligned;
    PrPathPolyline strokeRegion;
    PrPathPolyline combined;
    PrPathPolyline cut;
    PrPathPolyline outline;                 // a clip's outline, which cuts every triangle drawn within it
    PrPathPolyline centerline;
    PrPath* brushShape;         // the shape that a list brush fills, which clips its tiles
};

// The offset of a Create request's texels in the texel storage. It is used until the build ends and the storage stops
// moving.
struct PendingTexels {
    uint32_t request;
    size_t offset;
};

// The storage that one frame's arrays point into. A backend keeps the storage of destroyed frames for the next frames.
struct FrameStorage {
    Array<TrianglesVertex> vertices;
    Array<uint32_t> indices;
    Array<TrianglesDraw> draws;
    Array<TrianglesTextureRequest> requests;
    Array<uint8_t> texels;
    Array<PendingTexels> pending;
    uint32_t pathFills;                 // number of paths that the build tessellated
};

// A segment of a mapped stroke's centerline in target pixels, and the region whose paint it places. The region lies
// between the lines that bisect the turns at the segment's ends, and within the stroke's reach of the segment.
struct MapSegment {
    PrVec2 a;
    PrVec2 b;
    PrVec2 direction;          // unit
    float along;               // distance from its contour's start to `a`
    float contourLength;
    float left;                 // the half widths at its middle
    float right;
    bool startBound;            // true if a line through `a` along `startNormal` bounds it; false at an open end
    PrVec2 startNormal;
    bool endBound;
    PrVec2 endNormal;
    PrRect bounds;              // of the region
};

// A stroke whose brush is mapped along it or across it.
struct StrokeMap {
    Array<MapSegment> segments;
    PrAffine2 unitToBrush;       // the unit square to brush space
    PrDrawStrokeMapping mapping;
    float reach;                // how far from its centerline the stroke's outline can be, in target pixels
};

// A scissor rectangle, and the number of the walk's convex outlines that cut what draws within it.
struct Clip {
    PrIRect box;
    uint32_t outlines;
};

// A convex outline in target pixels, stored among the walk's clip points. The inside is on the side where `sign` times
// the turn is positive.
struct ClipOutline {
    uint32_t start;
    uint32_t count;
    float sign;
};

// The state of one build as it walks the commands.
struct Walk {
    Triangles* t;
    FrameStorage* b;
    PrIRect target;
    Array<PrDrawTransform> transforms;
    Array<Clip> clips;          // innermost last
    Array<ClipOutline> outlines;
    Array<PrVec2> clipPoints;
    Array<float> opacities;     // the groups' opacities multiplied, innermost last
    StrokeMap map;
    bool mapped;                // true if `map` places the paint of what is drawing
};

uint32_t packColor(float r, float g, float b, float a) {
    auto byte = [](float v) { return uint32_t(clampF32(v, 0, 1) * 255 + 0.5f); };
    return byte(r) | byte(g) << 8 | byte(b) << 16 | byte(a) << 24;
}

// Returns the color with its alpha scaled, packed with straight alpha.
uint32_t packStraight(PrColor c, float alpha) {
    return packColor(c.r, c.g, c.b, c.a * alpha);
}

// Returns the texture coordinate of the middle of the white block.
PrVec2 whiteUv(const Triangles* t) {
    float middle = float(kWhiteSize) / 2 / float(t->atlasSize);
    return PrVec2{middle, middle};
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures and requests

uint32_t newTexture(Triangles* t) {
    return ++t->nextTexture;
}

// Returns the texture of an atlas page. The texture is created at the end of the build.
uint32_t pageTexture(Triangles* t, uint32_t page) {
    while (t->pageTextures.count <= page)
        t->pageTextures.push(newTexture(t));
    return t->pageTextures.data[page];
}

void request(Walk& w, const TrianglesTextureRequest& r) {
    w.b->requests.push(r);
}

// Returns the texture of the image, which is created with straight texels the first time a build draws the image.
// Returns 0 for an image with no pixels.
uint32_t textureOfImage(Walk& w, const PrDrawImage* image) {
    Triangles* t = w.t;
    uint64_t id = prDrawImageId(image);
    uint32_t found = t->imageIndex.find(id, [&](uint32_t i) { return t->images.data[i].id; });
    if (found != kHashMissing) {
        t->images.data[found].lastUsed = t->build;
        return t->images.data[found].texture;
    }
    uint32_t width = prDrawImageExtent(image).x;
    uint32_t height = prDrawImageExtent(image).y;
    if (width == 0 || height == 0)
        return 0;
    size_t size = size_t(width) * height * 4;
    size_t offset = w.b->texels.count;
    // The frame's texels are addressed in 32 bits. An image beyond that limit draws nothing.
    if (size > UINT32_MAX - offset)
        return 0;
    w.b->texels.reserve(uint32_t(offset + size));
    w.b->texels.count = uint32_t(offset + size);
    uint8_t* bytes = w.b->texels.data + offset;
    // Premultiplied as the list's images are read, then converted to straight alpha.
    PrPixels pixels{bytes, size_t(width) * 4, {width, height}};
    prDrawImageWritePixels(image, pixels, PrPixelFormat_Rgba8, PrIVec2{0, 0});
    for (size_t i = 0; i < size; i += 4) {
        uint32_t a = bytes[i + 3];
        for (uint32_t k = 0; k < 3; ++k)
            bytes[i + k] = uint8_t(a == 0 ? 0 : (bytes[i + k] * 255 + a / 2) / a);
    }
    uint32_t texture = newTexture(t);
    uint32_t index = w.b->requests.count;
    request(w, TrianglesTextureRequest{
        TrianglesTextureOp_Create, texture, {0, 0},
        PrPixelView{nullptr, size_t(width) * 4, {width, height}},
    });
    w.b->pending.push(PendingTexels{index, offset});
    t->imageIndex.insert(id, t->images.count);
    t->images.push(CachedImage{id, texture, t->build});
    return texture;
}

// Returns true if the source samples texels, of an image or the host's texture, instead of playing a list.
bool sampled(const PrDrawSource& source) {
    return source.kind == PrDrawSourceKind_Image || source.kind == PrDrawSourceKind_Texture;
}

// A texture that a draw samples: a texture that the requests created, or a host texture.
struct DrawTexture {
    uint32_t texture;
    PrDrawTextureId hostTexture;

    bool exists() const { return texture != 0 || hostTexture != 0; }
};

// Returns the texture of a sampled source.
DrawTexture textureOfSource(Walk& w, const PrDrawSource& source) {
    if (source.kind == PrDrawSourceKind_Image)
        return DrawTexture{textureOfImage(w, source.image), 0};
    return DrawTexture{0, source.texture};
}

// Returns the size in texels of a sampled source's image or texture.
PrVec2 sourceSize(const PrDrawSource& source) {
    if (source.kind == PrDrawSourceKind_Image)
        return PrVec2{float(prDrawImageExtent(source.image).x), float(prDrawImageExtent(source.image).y)};
    return PrVec2{float(source.extent.x), float(source.extent.y)};
}

// ---------------------------------------------------------------------------------------------------------------------
// Draws

// Continues the last draw if it has the same texture, sampler and scissor, and starts one otherwise.
void useDraw(Walk& w, DrawTexture texture, TrianglesSampler sampler, const PrIRect& clip) {
    Array<TrianglesDraw>& draws = w.b->draws;
    TrianglesDraw draw{texture.hostTexture, texture.texture, sampler, clip, w.b->indices.count, 0};
    if (draws.count != 0) {
        TrianglesDraw& last = draws.last();
        bool same = last.hostTexture == draw.hostTexture && last.texture == draw.texture && last.sampler == sampler &&
            last.scissor.left == clip.left && last.scissor.top == clip.top &&
            last.scissor.right == clip.right && last.scissor.bottom == clip.bottom;
        if (same)
            return;
        if (last.indexCount == 0) {
            last = draw;
            return;
        }
    }
    draws.push(draw);
}

void addIndices(Walk& w, const uint32_t* indices, uint32_t count) {
    w.b->indices.append(indices, count);
    w.b->draws.last().indexCount += count;
}

const PrIRect& clipOf(const Walk& w) {
    return w.clips.last().box;
}

// Returns true if convex outlines cut what draws.
bool outlined(const Walk& w) {
    return w.clips.last().outlines != 0;
}

float opacityOf(const Walk& w) {
    return w.opacities.last();
}

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// The largest and smallest spacing, in target pixels, of the lines that cut a mesh whose paint is not affine in target
// pixels.
constexpr float kPaintGrid = 16;
constexpr float kPaintGridFinest = 3;

// The number of wedges that a concentric radial gradient is cut into. Across one wedge, the distance from the center
// along the wedge's middle differs from the true distance by less than a thousandth.
constexpr uint32_t kRadialWedges = 72;

// The maximum number of lines that cut one triangle along a gradient.
constexpr uint32_t kMaxCuts = 512;

// How far a line between two of a ramp's colors may stray from the colors between them, in 8-bit levels.
constexpr int kRampError = 2;

// The maximum number of points of a polygon cut from a triangle. A convex clip adds up to one point for each point of
// its outline.
constexpr uint32_t kMaxPiece = 256;

enum {
    PaintKind_Solid,
    PaintKind_Gradient,
    PaintKind_Image,
};
typedef uint8_t PaintKind;

// How a mesh is cut so that its vertices carry the paint.
enum {
    PaintCut_None,              // the paint is affine in target pixels
    PaintCut_Affine,            // a gradient whose parameter is affine: at the points where the ramp's colors turn
    PaintCut_Quadrants,         // a diamond gradient: along its axes, then as Affine within each quadrant
    PaintCut_Wedges,            // a conic gradient: along rays from its center
    PaintCut_Rings,             // a radial gradient with one center: into thin wedges, then as Affine along each
    PaintCut_Grid,              // on a grid of `grid` target pixels
};
typedef uint8_t PaintCut;

// The paint of a command, evaluated at each vertex. A target pixel's point in brush space is `fromDevice` applied to
// the pixel.
struct Paint {
    PaintKind kind;
    PrColor color;                        // Solid, Image: the color
    float alpha;                          // the groups' opacity and any fading
    const PrDrawBrush* PR_NULLABLE brush; // Gradient
    PrAffine2 fromDevice;                 // target pixels to brush space
    DrawTexture texture;
    TrianglesSampler sampler;
    PrAffine2 uvFromBrush;              // Image: brush space to texture coordinates
    bool clampUv;                       // Image: true if the coordinates stay within `uvBounds`
    PrRect uvBounds;
    bool pieceUv;                       // Image: true if the pieces carry the texture coordinates
    bool clipTile;                      // Image: true if nothing paints outside `tile`
    PrVec2 tile[4];                     // the tile in target pixels, a convex quadrilateral
    PaintCut cut;
    float t0;                           // Affine: the parameter is t0 + tx x + ty y in target pixels
    float tx;
    float ty;
    float grid;                         // Grid
    const CachedRamp* ramp;             // Gradient
};

PrVec2 brushPoint(const Paint& p, PrVec2 device) {
    return prAffine2MapPoint(p.fromDevice, device);
}

// Finds the breaks between pieces of the ramp whose colors lie close enough to lines between their ends. A piece grows
// while the colors at its quarters lie near the line between its ends.
void findBreaks(CachedRamp& r) {
    auto channel = [&](uint32_t i, uint32_t k) { return int((r.colors[i] >> (8 * k)) & 0xff); };
    auto near = [&](uint32_t start, uint32_t end, uint32_t i) {
        for (uint32_t k = 0; k < 4; ++k) {
            int a = channel(start, k);
            int line = a + (channel(end, k) - a) * int(i - start) / int(end - start);
            if (absF32(float(channel(i, k) - line)) > float(kRampError))
                return false;
        }
        return true;
    };
    r.breakCount = 0;
    r.breaks[r.breakCount++] = 0;
    uint32_t start = 0;
    for (uint32_t end = 2; end < kRampSize; ++end) {
        uint32_t span = end - start;
        bool fits = near(start, end, start + span / 4) && near(start, end, start + span / 2) &&
            near(start, end, start + span * 3 / 4) && near(start, end, end - 1);
        if (!fits) {
            start = end - 1;
            r.breaks[r.breakCount++] = float(start) / float(kRampSize - 1);
        }
    }
    r.breaks[r.breakCount++] = 1;
}

// Returns the gradient's colors for the color, which are created the first time this build asks for them. They stay in
// place until the build ends.
const CachedRamp* rampOf(Triangles* t, const PrDrawBrush* brush, PrColor color) {
    const uint32_t hash = uint32_t(prDrawGradientRampHash(brush, color));
    const uint32_t found = t->rampIndex.find(hash, [&](uint32_t i) {
        return prDrawGradientRampEquals(t->ramps.data[i]->brush, t->ramps.data[i]->color, brush, color);
    });
    if (found != kHashMissing)
        return t->ramps.data[found];
    t->rampIndex.insert(hash, t->rampCount);
    if (t->rampCount == t->ramps.count)
        t->ramps.push(alloc<CachedRamp>());
    CachedRamp& r = *t->ramps.data[t->rampCount++];
    r.brush = brush;
    r.color = color;
    // The colors are the ramp's at even steps of the parameter, from which findBreaks chooses where vertices go.
    Array<PrDrawRampPiece> pieces{};
    Array<PrColor> colors{};
    pieces.resize(prDrawGradientRampMaxPieces(brush->stopCount));
    colors.resize(prDrawGradientRampMaxColors(brush->stopCount, kRampSize));
    pieces.resize(prDrawGradientRamp(brush, color, PrColorSpace_Srgb, kRampSize, pieces.data, colors.data));
    uint32_t piece = 0;
    for (uint32_t i = 0; i < kRampSize; ++i) {
        float at = float(i) / float(kRampSize - 1);
        piece = prDrawRampPieceAt(pieces.data, at, piece);
        const PrColor k = prDrawRampColor(&pieces.data[piece], colors.data, at);
        float a = k.a;
        r.colors[i] = a > 0 ? packColor(k.r / a, k.g / a, k.b / a, a) : 0;
    }
    pieces.reset();
    colors.reset();
    findBreaks(r);
    r.rayCount = 0;
    uint32_t b = 0;
    for (uint32_t k = 0; k <= kConicWedges; ++k) {
        float step = float(k) / float(kConicWedges);
        for (; b < r.breakCount && r.breaks[b] <= step; ++b)
            r.rays[r.rayCount++] = r.breaks[b];
        if (r.rays[r.rayCount - 1] < step)
            r.rays[r.rayCount++] = step;
    }
    for (; b < r.breakCount; ++b)
        r.rays[r.rayCount++] = r.breaks[b];
    return &r;
}

// Chooses how a gradient's meshes are cut, for its placement in `fromDevice`.
void placeGradient(Paint& p) {
    const PrDrawBrush* brush = p.brush;
    const CachedRamp* ramp = p.ramp;
    switch (brush->kind) {
        case PrDrawBrushKind_Linear: {
            float at[3];
            PrVec2 probes[3] = {{0, 0}, {1, 0}, {0, 1}};
            for (uint32_t i = 0; i < 3; ++i)
                prDrawGradientParameter(brush, prAffine2MapPoint(p.fromDevice, probes[i]), &at[i]);
            p.cut = PaintCut_Affine;
            p.t0 = at[0];
            p.tx = at[1] - at[0];
            p.ty = at[2] - at[0];
            break;
        }
        case PrDrawBrushKind_Diamond:
            p.cut = PaintCut_Quadrants;
            break;
        case PrDrawBrushKind_Conic:
            p.cut = PaintCut_Wedges;
            break;
        case PrDrawBrushKind_Radial:
            if (brush->radial.start.x == brush->radial.end.x && brush->radial.start.y == brush->radial.end.y &&
                    brush->radial.endRadius > brush->radial.startRadius) {
                p.cut = PaintCut_Rings;
                break;
            }
            [[fallthrough]];
        default: {
            // The grid is finer if the colors turn within fewer pixels. Its spacing is half the number of target pixels
            // between the closest breaks.
            float gap = 1;
            for (uint32_t i = 1; i < ramp->breakCount; ++i) {
                if (ramp->breaks[i] > ramp->breaks[i - 1])
                    gap = minF32(gap, ramp->breaks[i] - ramp->breaks[i - 1]);
            }
            float perUnit = prAffine2MaxScale(p.fromDevice);
            float pixels = perUnit > 0 ? brush->radial.endRadius / perUnit : kPaintGrid;
            p.cut = PaintCut_Grid;
            p.grid = clampF32(pixels * gap / 2, kPaintGridFinest, kPaintGrid);
            break;
        }
    }
}

// Computes the paint of a brush times a color, for a command whose fill geometry has `bounds` in the current space.
// Returns false if it paints nothing.
bool makePaint(Walk& w, const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds, Paint* out) {
    const PrDrawTransform& x = w.transforms.last();
    out->kind = PaintKind_Solid;
    out->color = color;
    out->alpha = opacityOf(w);
    out->brush = brush;
    out->texture = DrawTexture{pageTexture(w.t, 0), 0};
    out->sampler = TrianglesSampler_LinearClamp;
    out->clampUv = false;
    out->clipTile = false;
    out->cut = PaintCut_None;
    if (brush == nullptr)
        return color.a > 0;
    if (!prDrawBrushFromTarget(brush, x.affine, bounds, &out->fromDevice))
        return false;
    if (brush->kind == PrDrawBrushKind_Image) {
        const PrDrawSource& source = brush->image.source;
        const PrDrawTiles& tiles = brush->image.tiles;
        const PrRect& tile = tiles.tile;
        if (!sampled(source) || !(tile.right > tile.left && tile.bottom > tile.top))
            return false;
        DrawTexture texture = textureOfSource(w, source);
        if (!texture.exists())
            return false;
        // One sampler serves both axes, chosen by the horizontal mode.
        bool nearest = (brush->image.sampling & PrDrawSamplingFlag_Kind) == PrDrawSampling_Nearest;
        out->kind = PaintKind_Image;
        out->texture = texture;
        switch (tiles.x) {
            case PrDrawExtend_Repeat:
                out->sampler = nearest ? TrianglesSampler_NearestRepeat : TrianglesSampler_LinearRepeat;
                break;
            case PrDrawExtend_Reflect:
                out->sampler = nearest ? TrianglesSampler_NearestMirror : TrianglesSampler_LinearMirror;
                break;
            default:
                out->sampler = nearest ? TrianglesSampler_NearestClamp : TrianglesSampler_LinearClamp;
                break;
        }
        // The tile spans the source's texels.
        PrVec2 size = sourceSize(source);
        float width = size.x;
        float height = size.y;
        PrAffine2 toSource = prAffine2Concat(prAffine2Translate(PrVec2{-tile.left, -tile.top}), prAffine2Scale(PrVec2{
            prRectWidth(source.rect) / prRectWidth(tile),
            prRectHeight(source.rect) / prRectHeight(tile),
        }));
        toSource = prAffine2Concat(toSource, prAffine2Translate(PrVec2{source.rect.left, source.rect.top}));
        out->uvFromBrush = prAffine2Concat(toSource, prAffine2Scale(PrVec2{1 / width, 1 / height}));
        PrAffine2 toDevice{};
        if (tiles.x == PrDrawExtend_None && tiles.y == PrDrawExtend_None && prAffine2Invert(out->fromDevice, &toDevice)) {
            out->clipTile = true;
            PrVec2 corners[4] = {
                {tile.left, tile.top}, {tile.right, tile.top}, {tile.right, tile.bottom},
                {tile.left, tile.bottom},
            };
            for (uint32_t i = 0; i < 4; ++i)
                out->tile[i] = prAffine2MapPoint(toDevice, corners[i]);
        }
        return color.a > 0;
    }
    out->kind = PaintKind_Gradient;
    out->ramp = rampOf(w.t, brush, color);
    placeGradient(*out);
    return true;
}

// Returns the ramp's color at a parameter before it extends, with its alpha scaled.
uint32_t rampColor(const Paint& p, float t, float alpha) {
    if (!prDrawGradientExtend(p.brush->extend, &t))
        return 0;
    uint32_t c = p.ramp->colors[uint32_t(clampF32(t, 0, 1) * float(kRampSize - 1) + 0.5f)];
    uint32_t a = uint32_t(float(c >> 24) * clampF32(alpha, 0, 1) + 0.5f);
    return (c & 0xffffffu) | a << 24;
}

uint32_t colorAt(const Paint& p, PrVec2 device, float coverage) {
    float alpha = p.alpha * coverage;
    if (p.kind != PaintKind_Gradient)
        return packStraight(p.color, alpha);
    float t = 0;
    if (!prDrawGradientParameter(p.brush, brushPoint(p, device), &t))
        return 0;
    return rampColor(p, t, alpha);
}

PrVec2 uvAt(const Walk& w, const Paint& p, PrVec2 device) {
    if (p.kind != PaintKind_Image)
        return whiteUv(w.t);
    PrVec2 uv = prAffine2MapPoint(p.uvFromBrush, brushPoint(p, device));
    if (p.clampUv) {
        uv.x = clampF32(uv.x, p.uvBounds.left, p.uvBounds.right);
        uv.y = clampF32(uv.y, p.uvBounds.top, p.uvBounds.bottom);
    }
    return uv;
}

// A point of a polygon cut from a triangle: its coverage, and the value that the cuts read.
struct Piece {
    PrVec2 p;
    float coverage;
    float value;
    PrVec2 uv;                 // used if the paint's texture coordinates come with the points
};

// Writes the part of the polygon on the side of a line where `side(point)` is at least zero, interpolating the values
// that the points carry. Returns the number of points written to `out`.
template <typename Side>
uint32_t keepSide(const Piece* in, uint32_t count, Side side, Piece* out) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count && kept + 2 <= kMaxPiece; ++i) {
        const Piece& a = in[i];
        const Piece& b = in[i + 1 < count ? i + 1 : 0];
        float sa = side(a);
        float sb = side(b);
        if (sa >= 0)
            out[kept++] = a;
        if ((sa >= 0) != (sb >= 0)) {
            float t = sa / (sa - sb);
            out[kept++] = Piece{
                a.p + (b.p - a.p) * t,
                a.coverage + (b.coverage - a.coverage) * t, a.value + (b.value - a.value) * t,
                a.uv + (b.uv - a.uv) * t,
            };
        }
    }
    return kept;
}

// Writes the part of the polygon inside the convex outline. The outline's inside is on the side where `sign` times the
// turn is positive.
uint32_t keepInside(Piece* polygon, uint32_t count, const PrVec2* outline, uint32_t points, float sign) {
    Piece next[kMaxPiece];
    for (uint32_t e = 0; e < points && count != 0; ++e) {
        PrVec2 a = outline[e];
        PrVec2 b = outline[e + 1 < points ? e + 1 : 0];
        count = keepSide(polygon, count, [&](const Piece& q) {
            return sign * ((b.x - a.x) * (q.p.y - a.y) - (b.y - a.y) * (q.p.x - a.x));
        }, next);
        memCopy(polygon, next, sizeof(Piece) * count);
    }
    return count;
}

// Draws the polygon as a fan in the current draw. A piece of a linear gradient between the cuts `low` and `high` takes
// its colors from inside that span, so that the colors on a cut belong to the span being drawn.
void drawPiece(Walk& w, const Piece* piece, uint32_t count, const Paint& paint, float low, float high) {
    if (count < 3)
        return;
    uint32_t base = w.b->vertices.count;
    for (uint32_t i = 0; i < count; ++i) {
        PrVec2 p = piece[i].p;
        uint32_t color;
        if (paint.kind == PaintKind_Gradient && paint.cut != PaintCut_Grid) {
            float inset = isFiniteF32(high - low) ? (high - low) / 100 : 1e-4f;
            float t = clampF32(piece[i].value, low + inset, high - inset);
            color = rampColor(paint, t, paint.alpha * piece[i].coverage);
        } else {
            color = colorAt(paint, p, piece[i].coverage);
        }
        w.b->vertices.push(TrianglesVertex{p, paint.pieceUv ? piece[i].uv : uvAt(w, paint, p), color});
    }
    for (uint32_t i = 1; i + 1 < count; ++i) {
        uint32_t indices[3] = {base, base + i, base + i + 1};
        addIndices(w, indices, 3);
    }
}

// Cuts the polygon at each of the ascending values of its points' `value`, and passes each piece to `each` with the
// values that bound it. Each cut divides what remains of the polygon into both sides of it, so neighboring pieces share
// their points exactly.
template <typename Each>
void cutAt(const Piece* polygon, uint32_t count, const float* cuts, uint32_t cutCount, Each each) {
    Piece rest[kMaxPiece];
    Piece above[kMaxPiece];
    Piece piece[kMaxPiece];
    memCopy(rest, polygon, sizeof(Piece) * count);
    uint32_t n = count;
    for (uint32_t i = 0; i < cutCount && n >= 3; ++i) {
        float cut = cuts[i];
        uint32_t m = keepSide(rest, n, [&](const Piece& q) { return cut - q.value; }, piece);
        n = keepSide(rest, n, [&](const Piece& q) { return q.value - cut; }, above);
        memCopy(rest, above, sizeof(Piece) * n);
        if (m >= 3)
            each(piece, m, i == 0 ? -kInfinityF32 : cuts[i - 1], cut);
    }
    if (n >= 3)
        each(rest, n, cutCount == 0 ? -kInfinityF32 : cuts[cutCount - 1], kInfinityF32);
}

// Writes the values at which a linear gradient's parameter, before it extends, reaches one of the ramp's breaks,
// strictly between `low` and `high`, in ascending order. Returns the number of values.
uint32_t gradientCuts(const Paint& p, float low, float high, float* out) {
    uint32_t count = 0;
    auto add = [&](float v) {
        if (v > low && v < high && count < kMaxCuts && (count == 0 || v > out[count - 1]))
            out[count++] = v;
    };
    if (p.brush->extend == PrDrawExtend_Pad || p.brush->extend == PrDrawExtend_None) {
        // None paints nothing past the range, so the range's ends are also cuts.
        bool none = p.brush->extend == PrDrawExtend_None;
        if (none)
            add(0);
        for (uint32_t i = 0; i < p.ramp->breakCount; ++i)
            add(p.ramp->breaks[i]);
        if (none)
            add(1);
        return count;
    }
    float first = floorF32(low);
    for (float k = first; k <= high && count < kMaxCuts; k += 1) {
        bool mirrored = p.brush->extend == PrDrawExtend_Reflect && fmodF32(absF32(k), 2) == 1;
        const CachedRamp& r = *p.ramp;
        for (uint32_t i = 0; i < r.breakCount; ++i)
            add(mirrored ? k + 1 - r.breaks[r.breakCount - 1 - i] : k + r.breaks[i]);
    }
    return count;
}

// Writes the multiples of `spacing` strictly between `low` and `high`, in ascending order. Returns the number of
// multiples.
uint32_t gridCuts(float spacing, float low, float high, float* out) {
    uint32_t count = 0;
    for (float v = floorF32(low / spacing + 1) * spacing; v < high && count < kMaxCuts; v += spacing)
        out[count++] = v;
    return count;
}

// Draws the polygon of a gradient whose parameter over it is t0 + tx x + ty y, cut where the ramp's colors turn.
void drawAffine(Walk& w, Piece* polygon, uint32_t count, const Paint& paint, float t0, float tx, float ty) {
    float low = kInfinityF32;
    float high = -kInfinityF32;
    for (uint32_t i = 0; i < count; ++i) {
        Piece& q = polygon[i];
        q.value = t0 + tx * q.p.x + ty * q.p.y;
        low = minF32(low, q.value);
        high = maxF32(high, q.value);
    }
    float cuts[kMaxCuts];
    uint32_t n = gradientCuts(paint, low, high, cuts);
    cutAt(polygon, count, cuts, n, [&](const Piece* piece, uint32_t m, float from, float to) {
        drawPiece(w, piece, m, paint, from, to);
    });
}

// Draws the polygon of a diamond gradient, cut along its axes. Within each quadrant, the gradient's parameter is
// affine.
void drawQuadrants(Walk& w, Piece* polygon, uint32_t count, const Paint& paint) {
    const PrDrawBrush& brush = *paint.brush;
    const PrAffine2& f = paint.fromDevice;
    PrVec2 c = brush.diamond.center;
    PrVec2 r = brush.diamond.radii;
    // The offsets from the center along the brush's axes are affine in target pixels.
    for (uint32_t i = 0; i < count; ++i)
        polygon[i].value = f.a * polygon[i].p.x + f.c * polygon[i].p.y + f.e - c.x;
    float zero = 0;
    cutAt(polygon, count, &zero, 1, [&](const Piece* half, uint32_t m, float, float xHigh) {
        Piece rows[kMaxPiece];
        for (uint32_t i = 0; i < m; ++i) {
            rows[i] = half[i];
            rows[i].value = f.b * rows[i].p.x + f.d * rows[i].p.y + f.f - c.y;
        }
        float sx = xHigh == 0 ? -1 : 1;
        cutAt(rows, m, &zero, 1, [&](const Piece* quarter, uint32_t k, float, float yHigh) {
            float sy = yHigh == 0 ? -1 : 1;
            // t = sx (x - cx) / rx + sy (y - cy) / ry, with x and y the brush point.
            float tx = sx * f.a / r.x + sy * f.b / r.y;
            float ty = sx * f.c / r.x + sy * f.d / r.y;
            float t0 = sx * (f.e - c.x) / r.x + sy * (f.f - c.y) / r.y;
            Piece piece[kMaxPiece];
            memCopy(piece, quarter, sizeof(Piece) * k);
            drawAffine(w, piece, k, paint, t0, tx, ty);
        });
    });
}

// Which side of a ray from `center` in brush space a piece's point is on: positive if the brush point is
// counterclockwise of the ray, as atan2 measures angles.
struct RaySide {
    PrAffine2 fromDevice;
    PrVec2 center;
    PrVec2 direction;

    float operator()(const Piece& q) const {
        PrVec2 b = prAffine2MapPoint(fromDevice, q.p);
        return direction.x * (b.y - center.y) - direction.y * (b.x - center.x);
    }
};

// Returns the side of the ray from `center` at `angle`.
RaySide raySide(const Paint& paint, PrVec2 center, float angle) {
    return RaySide{.fromDevice = paint.fromDevice, .center = center, .direction = {cosF32(angle), sinF32(angle)}};
}

// Cuts the polygon along lines through a center in brush space, at each of the angles. The angles ascend through a
// turn, from the first to the last, and one of them is half a turn past the first. Passes the wedge between each pair
// of neighboring angles to `each` with the index of the first angle. As with cutAt, neighbors share their points
// exactly.
template <typename Each>
void cutWedges(const Piece* polygon, uint32_t count, const Paint& paint, PrVec2 c, const float* angles, uint32_t n, Each each) {
    uint32_t half = 0;
    while (half + 1 < n && angles[half] < angles[0] + kPiF32 - 1e-4f)
        ++half;
    RaySide first = raySide(paint, c, angles[0]);
    Piece halves[2][kMaxPiece];
    uint32_t counts[2] = {
        keepSide(polygon, count, first, halves[0]),
        keepSide(polygon, count, [&](const Piece& q) { return -first(q); }, halves[1]),
    };
    uint32_t starts[2] = {0, half};
    uint32_t ends[2] = {half, n - 1};
    Piece piece[kMaxPiece];
    Piece next[kMaxPiece];
    for (uint32_t h = 0; h < 2; ++h) {
        Piece* rest = halves[h];
        uint32_t remaining = counts[h];
        for (uint32_t k = starts[h] + 1; k < ends[h] && remaining >= 3; ++k) {
            RaySide ray = raySide(paint, c, angles[k]);
            uint32_t m = keepSide(rest, remaining, [&](const Piece& q) { return -ray(q); }, piece);
            remaining = keepSide(rest, remaining, ray, next);
            memCopy(rest, next, sizeof(Piece) * remaining);
            if (m >= 3)
                each(piece, m, k - 1);
        }
        if (remaining >= 3)
            each(rest, remaining, ends[h] - 1);
    }
}

// Draws the polygon of a conic gradient, cut into wedges between rays from its center. Each wedge is drawn with the
// parameters within its own span. The parameter is constant along a ray, and changes fastest across a wedge near the
// center. Each wedge is therefore cut across at one target pixel from the center, and at each doubling of that
// distance. The sliver at the center, where the parameter has no value, takes the middle of the wedge's span.
void drawWedges(Walk& w, Piece* polygon, uint32_t count, const Paint& paint) {
    const PrDrawBrush& brush = *paint.brush;
    const PrAffine2& f = paint.fromDevice;
    PrVec2 c = brush.conic.center;
    float near = prAffine2MaxScale(f);
    const CachedRamp& ramp = *paint.ramp;
    float angles[kRampSize + kConicWedges + 1];
    for (uint32_t i = 0; i < ramp.rayCount; ++i)
        angles[i] = brush.conic.angle + 2 * kPiF32 * ramp.rays[i];
    cutWedges(polygon, count, paint, c, angles, ramp.rayCount, [&](const Piece* piece, uint32_t n, uint32_t index) {
        float low = ramp.rays[index];
        float high = ramp.rays[index + 1];
        float bisector = (angles[index] + angles[index + 1]) / 2;
        PrVec2 u{cosF32(bisector), sinF32(bisector)};
        Piece wedge[kMaxPiece];
        float far = 0;
        for (uint32_t k = 0; k < n; ++k) {
            wedge[k] = piece[k];
            PrVec2 b = prAffine2MapPoint(f, piece[k].p);
            wedge[k].value = u.x * (b.x - c.x) + u.y * (b.y - c.y);
            far = maxF32(far, wedge[k].value);
        }
        float cuts[64];
        uint32_t cutCount = 0;
        for (float d = near; d < far && cutCount < 64; d *= 2)
            cuts[cutCount++] = d;
        cutAt(wedge, n, cuts, cutCount, [&](const Piece* part, uint32_t m, float, float) {
            Piece colored[kMaxPiece];
            for (uint32_t k = 0; k < m; ++k) {
                colored[k] = part[k];
                PrVec2 b = prAffine2MapPoint(f, part[k].p);
                float t = (low + high) / 2;
                if (absF32(b.x - c.x) + absF32(b.y - c.y) > near * 1e-3f) {
                    prDrawGradientParameter(&brush, b, &t);
                    // A point on the ray at the turn's start belongs to either end of the turn. It takes the end that
                    // is nearer the wedge.
                    float middle = (low + high) / 2;
                    t = t - middle > 0.5f ? t - 1 : middle - t > 0.5f ? t + 1 : t;
                }
                colored[k].value = t;
            }
            drawPiece(w, colored, m, paint, low, high);
        });
    });
}

// Draws the polygon of a radial gradient with one center, cut into thin wedges. Along each wedge, the parameter is the
// distance along the wedge's middle, which is affine.
void drawRings(Walk& w, Piece* polygon, uint32_t count, const Paint& paint) {
    const PrDrawBrush& brush = *paint.brush;
    const PrAffine2& f = paint.fromDevice;
    PrVec2 c = brush.radial.end;
    float r0 = brush.radial.startRadius;
    float span = brush.radial.endRadius - r0;
    float angles[kRadialWedges + 1];
    for (uint32_t i = 0; i <= kRadialWedges; ++i)
        angles[i] = 2 * kPiF32 * float(i) / float(kRadialWedges);
    cutWedges(polygon, count, paint, c, angles, kRadialWedges + 1, [&](const Piece* piece, uint32_t n, uint32_t i) {
        // t = (u . (b - c) - r0) / span, with u along the middle and b the brush point.
        float middle = (angles[i] + angles[i + 1]) / 2;
        PrVec2 u{cosF32(middle), sinF32(middle)};
        float tx = (u.x * f.a + u.y * f.b) / span;
        float ty = (u.x * f.c + u.y * f.d) / span;
        float t0 = (u.x * (f.e - c.x) + u.y * (f.f - c.y) - r0) / span;
        Piece wedge[kMaxPiece];
        memCopy(wedge, piece, sizeof(Piece) * n);
        drawAffine(w, wedge, n, paint, t0, tx, ty);
    });
}

// Draws a convex polygon in the current draw. The clips' convex outlines cut it first, then a tile. Then a gradient's
// polygon is cut where its ramp's colors turn, and the polygon of other paint that is not affine in target pixels is
// cut on a grid, so that the vertices carry the paint.
void drawPolygon(Walk& w, Piece* polygon, uint32_t count, const Paint& paint) {
    for (uint32_t i = 0; i < w.clips.last().outlines && count >= 3; ++i) {
        const ClipOutline& o = w.outlines.data[i];
        count = keepInside(polygon, count, w.clipPoints.data + o.start, o.count, o.sign);
    }
    if (paint.clipTile)
        count = keepInside(polygon, count, paint.tile, 4, 1);
    if (count < 3)
        return;
    switch (paint.cut) {
        case PaintCut_Affine:
            drawAffine(w, polygon, count, paint, paint.t0, paint.tx, paint.ty);
            return;
        case PaintCut_Quadrants:
            drawQuadrants(w, polygon, count, paint);
            return;
        case PaintCut_Wedges:
            drawWedges(w, polygon, count, paint);
            return;
        case PaintCut_Rings:
            drawRings(w, polygon, count, paint);
            return;
        case PaintCut_Grid:
            break;
        default:
            drawPiece(w, polygon, count, paint, -kInfinityF32, kInfinityF32);
            return;
    }
    float low = kInfinityF32;
    float high = -kInfinityF32;
    for (uint32_t i = 0; i < count; ++i) {
        polygon[i].value = polygon[i].p.x;
        low = minF32(low, polygon[i].value);
        high = maxF32(high, polygon[i].value);
    }
    float cuts[kMaxCuts];
    uint32_t n = gridCuts(paint.grid, low, high, cuts);
    cutAt(polygon, count, cuts, n, [&](const Piece* column, uint32_t m, float, float) {
        Piece rows[kMaxPiece];
        float top = kInfinityF32;
        float bottom = -kInfinityF32;
        for (uint32_t i = 0; i < m; ++i) {
            rows[i] = column[i];
            rows[i].value = rows[i].p.y;
            top = minF32(top, rows[i].value);
            bottom = maxF32(bottom, rows[i].value);
        }
        float rowCuts[kMaxCuts];
        uint32_t k = gridCuts(paint.grid, top, bottom, rowCuts);
        cutAt(rows, m, rowCuts, k, [&](const Piece* piece, uint32_t pieceCount, float, float) {
            drawPiece(w, piece, pieceCount, paint, -kInfinityF32, kInfinityF32);
        });
    });
}

// Returns true if the mesh's vertices carry the paint as they are.
bool paintsAffine(const Paint& p) {
    return p.cut == PaintCut_None && !p.clipTile;
}

// Returns the transform from target pixels to the unit square along the segment. The square's coordinates are the
// fraction of the contour's length and the fraction of the way across from the left side. Beyond an open end, `held`
// holds the distance along the contour at that end.
PrAffine2 unitFromDevice(const StrokeMap& m, const MapSegment& s, const float* PR_NULLABLE held) {
    PrVec2 d = s.direction;
    PrVec2 right{-d.y, d.x};
    float width = s.left + s.right;
    float along0 = s.along - (d.x * s.a.x + d.y * s.a.y);
    PrAffine2 along{d.x / s.contourLength, 0, d.y / s.contourLength, 0, along0 / s.contourLength, 0};
    if (held != nullptr)
        along = PrAffine2{0, 0, 0, 0, *held / s.contourLength, 0};
    float across0 = width > 0 ? (s.left - (right.x * s.a.x + right.y * s.a.y)) / width : 0.5f;
    float ax = width > 0 ? right.x / width : 0;
    float ay = width > 0 ? right.y / width : 0;
    if (m.mapping == PrDrawStrokeMapping_Along)
        return PrAffine2{along.a, ax, along.c, ay, along.e, across0};
    return PrAffine2{ax, along.a, ay, along.c, across0, along.e};
}

// Draws a triangle of a mapped stroke, cut by the regions of the centerline's segments. Each piece has the brush placed
// by its segment.
void drawMapped(Walk& w, const Piece* triangle, const Paint& paint) {
    const StrokeMap& m = w.map;
    PrRect box{triangle[0].p.x, triangle[0].p.y, triangle[0].p.x, triangle[0].p.y};
    for (uint32_t k = 1; k < 3; ++k) {
        box.left = minF32(box.left, triangle[k].p.x);
        box.top = minF32(box.top, triangle[k].p.y);
        box.right = maxF32(box.right, triangle[k].p.x);
        box.bottom = maxF32(box.bottom, triangle[k].p.y);
    }
    for (uint32_t i = 0; i < m.segments.count; ++i) {
        const MapSegment& s = m.segments.data[i];
        if (s.bounds.left > box.right || s.bounds.right < box.left || s.bounds.top > box.bottom || s.bounds.bottom < box.top)
            continue;
        Piece polygon[kMaxPiece];
        Piece next[kMaxPiece];
        memCopy(polygon, triangle, sizeof(Piece) * 3);
        uint32_t count = 3;
        auto keep = [&](PrVec2 origin, PrVec2 normal, float offset) {
            count = keepSide(polygon, count, [&](const Piece& q) {
                return (q.p.x - origin.x) * normal.x + (q.p.y - origin.y) * normal.y + offset;
            }, next);
            memCopy(polygon, next, sizeof(Piece) * count);
        };
        PrVec2 right{-s.direction.y, s.direction.x};
        if (s.startBound)
            keep(s.a, s.startNormal, 0);
        if (s.endBound && count >= 3)
            keep(s.b, -s.endNormal, 0);
        if (count >= 3)
            keep(s.a, right, m.reach);
        if (count >= 3)
            keep(s.a, -right, m.reach);
        if (count < 3)
            continue;
        auto place = [&](Piece* piece, uint32_t n, const float* PR_NULLABLE held) {
            Paint placed = paint;
            placed.fromDevice = prAffine2Concat(unitFromDevice(m, s, held), m.unitToBrush);
            if (placed.kind == PaintKind_Gradient)
                placeGradient(placed);
            drawPolygon(w, piece, n, placed);
        };
        // Past an open end, such as over a cap, the distance along the contour stays at the end's distance.
        for (uint32_t k = 0; k < count; ++k)
            polygon[k].value = (polygon[k].p.x - s.a.x) * s.direction.x + (polygon[k].p.y - s.a.y) * s.direction.y;
        float span = (s.b.x - s.a.x) * s.direction.x + (s.b.y - s.a.y) * s.direction.y;
        float ends[2] = {0, span};
        float start = s.along;
        float end = s.along + span;
        cutAt(polygon, count, ends, 2, [&](const Piece* part, uint32_t n, float low, float high) {
            Piece piece[kMaxPiece];
            memCopy(piece, part, sizeof(Piece) * n);
            if (high == 0 && !s.startBound)
                place(piece, n, &start);
            else if (low == span && !s.endBound)
                place(piece, n, &end);
            else
                place(piece, n, nullptr);
        });
    }
}

// Draws the mesh in target pixels with the paint.
void drawMesh(Walk& w, const PrPathMesh& mesh, const Paint& paint) {
    if (mesh.indexCount == 0 || prIRectIsEmpty(clipOf(w)))
        return;
    useDraw(w, paint.texture, paint.sampler, clipOf(w));
    if (w.mapped) {
        for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3) {
            Piece triangle[3];
            for (uint32_t k = 0; k < 3; ++k) {
                uint32_t v = mesh.indices[i + k];
                triangle[k] = Piece{mesh.points[v], mesh.coverage[v], 0, PrVec2{0, 0}};
            }
            drawMapped(w, triangle, paint);
        }
        return;
    }
    if (!paintsAffine(paint) || outlined(w)) {
        for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3) {
            Piece polygon[kMaxPiece];
            for (uint32_t k = 0; k < 3; ++k) {
                uint32_t v = mesh.indices[i + k];
                polygon[k] = Piece{mesh.points[v], mesh.coverage[v], 0, PrVec2{0, 0}};
            }
            drawPolygon(w, polygon, 3, paint);
        }
        return;
    }
    uint32_t base = w.b->vertices.count;
    for (uint32_t i = 0; i < mesh.vertexCount; ++i) {
        PrVec2 p = mesh.points[i];
        w.b->vertices.push(TrianglesVertex{p, uvAt(w, paint, p), colorAt(paint, p, mesh.coverage[i])});
    }
    uint32_t start = w.b->indices.count;
    addIndices(w, mesh.indices, mesh.indexCount);
    for (uint32_t i = start; i < w.b->indices.count; ++i)
        w.b->indices.data[i] += base;
}

// Flattens the path into `out` in target pixels, under the current transform, and returns `out`. A fill's curves keep
// their area.
const PrPathPolyline& land(Walk& w, const PrPath* path, PrPathFlattening flattening, PrPathPolyline& out) {
    const PrDrawTransform& x = w.transforms.last();
    prPathPolylineClear(&out);
    prPathFlatten(path, x.affine, kTolerance, flattening, &out);
    return out;
}

// Fills a polyline in target pixels with a mesh of triangles.
void fillDevice(Walk& w, const PrPathPolyline& polyline, PrPathFillRule rule, bool antialias, const Paint& paint) {
    ++w.b->pathFills;
    PrPathMesh mesh = prPathTessellatorFill(w.t->tessellator, &polyline, rule, antialias ? kFringe : 0);
    drawMesh(w, mesh, paint);
}

// Fills a path in the current space.
void fillPath(Walk& w, const PrPath* path, PrPathFillRule rule, bool antialias, const Paint& paint) {
    fillDevice(w, land(w, path, PrPathFlattening_Balanced, w.t->polyline), rule, antialias, paint);
}

// Strokes a path in the current space. A stroke narrower than a pixel is drawn a pixel wide, fading with its width.
void strokePath(Walk& w, const PrPath* shape, const PrDrawStroke* stroke, bool antialias, Paint paint) {
    Triangles* t = w.t;
    const PrDrawTransform& x = w.transforms.last();
    if (!(stroke->shape.width > 0))
        return;
    float scale = prAffine2MaxScale(x.affine);
    if (!(scale > 0))
        return;
    PrDrawStroke s = *stroke;
    float device = s.nonScaling ? s.shape.width : s.shape.width * scale;
    if (antialias && device < 1) {
        paint.alpha *= device;
        s.shape.width = s.nonScaling ? 1 : s.shape.width / device;
    }
    prPathPolylineClear(&t->fill);
    prPathPolylineClear(&t->aligned);
    prPathPolylineClear(&t->strokeRegion);
    prDrawLowerStroke(shape, &s, x.affine, kTolerance, &t->fill, &t->aligned, &t->strokeRegion);
    if (t->aligned.pointCount != 0) {
        // Cuts the aligned outline to the region's interior or exterior.
        prPathPolylineClear(&t->combined);
        PrPathPolylineOp op =
            s.align == PrDrawStrokeAlign_Inside ? PrPathPolylineOp_Intersect : PrPathPolylineOp_Difference;
        prPathPolylineCombine(&t->aligned, PrPathFillRule_NonZero, &t->strokeRegion, PrPathFillRule_NonZero, op,
            &t->combined);
        fillDevice(w, t->combined, PrPathFillRule_NonZero, antialias, paint);
    }
    if (t->fill.pointCount != 0)
        fillDevice(w, t->fill, PrPathFillRule_NonZero, antialias, paint);
}

// ---------------------------------------------------------------------------------------------------------------------
// Lists

void runCommands(Walk& w, const PrDrawCommand* commands, uint32_t count);
void pushClip(Walk& w, const PrDrawCommand& c);
void popClip(Walk& w);

// Plays the list's `area` mapped onto `destination` in the current space, clipped to the destination, with its alpha
// scaled by `alpha`. The list draws its own commands instead of an image of them.
void drawListRect(Walk& w, const PrDrawList* drawn, PrRect area, PrRect destination, float alpha) {
    float width = prRectWidth(area);
    float height = prRectHeight(area);
    if (!(width > 0 && height > 0) || prRectIsEmpty(destination))
        return;
    PrDrawCommand clip{};
    clip.kind = PrDrawCommandKind_PushClipRect;
    clip.rect = destination;
    pushClip(w, clip);
    PrDrawCommand place{};
    place.kind = PrDrawCommandKind_PushTransformAffine2;
    place.affine =
        prDAffine2FromAffine2(prAffine2Concat(prAffine2Concat(prAffine2Translate(PrVec2{-area.left, -area.top}),
            prAffine2Scale(PrVec2{prRectWidth(destination) / width, prRectHeight(destination) / height})),
            prAffine2Translate(PrVec2{destination.left, destination.top})));
    PrDrawTransform next = prDrawTransformPush(&w.transforms.last(), &place);
    w.transforms.push(next);
    w.opacities.push(opacityOf(w) * clampF32(alpha, 0, 1));
    if (!prIRectIsEmpty(clipOf(w)))
        runCommands(w, prDrawListCommands(drawn), prDrawListCommandCount(drawn));
    w.opacities.pop();
    w.transforms.pop();
    popClip(w);
}

// Fills the shape with a list brush. The shape clips, and the list's area plays once in each tile that the clip's
// bounds reach. If the brush reflects, alternate tiles are mirrored. A tile that is clamped plays once.
void drawListBrush(Walk& w, const PrDrawCommand& c) {
    const PrDrawBrush& brush = *c.brush;
    const PrDrawTiles& tiles = brush.image.tiles;
    const PrDrawTransform& x = w.transforms.last();
    PrRect tile = tiles.tile;
    PrRect area = brush.image.source.rect;
    if (!(tile.right > tile.left && tile.bottom > tile.top) || !(c.color.a > 0))
        return;
    PrAffine2 fromDevice{};
    PrAffine2 toDevice{};
    PrAffine2 userFromDevice{};
    if (!prDrawBrushFromTarget(&brush, x.affine, prDrawFillBounds(&c), &fromDevice) ||
            !prAffine2Invert(fromDevice, &toDevice) || !prAffine2Invert(x.affine, &userFromDevice))
        return;
    // The shape clips in the command's space, and the tiles play in brush space.
    prPathClear(w.t->brushShape);
    prDrawShapePath(&c, w.t->brushShape);
    PrDrawCommand clip{};
    clip.kind = PrDrawCommandKind_PushClipPath;
    clip.rule = c.rule;
    clip.path = w.t->brushShape;
    pushClip(w, clip);
    PrIRect box = clipOf(w);
    PrDrawCommand toBrush{};
    toBrush.kind = PrDrawCommandKind_PushTransformAffine2;
    toBrush.affine = prDAffine2FromAffine2(prAffine2Concat(toDevice, userFromDevice));
    PrDrawTransform brushSpace = prDrawTransformPush(&x, &toBrush);
    w.transforms.push(brushSpace);
    // The tiles that the clip's bounds reach, in brush space.
    PrVec2 corners[4] = {
        {float(box.left), float(box.top)}, {float(box.right), float(box.top)},
        {float(box.right), float(box.bottom)}, {float(box.left), float(box.bottom)},
    };
    PrRect reach{kInfinityF32, kInfinityF32, -kInfinityF32, -kInfinityF32};
    for (const PrVec2& corner : corners) {
        PrVec2 b = prAffine2MapPoint(fromDevice, corner);
        reach = PrRect{
            minF32(reach.left, b.x), minF32(reach.top, b.y), maxF32(reach.right, b.x),
            maxF32(reach.bottom, b.y),
        };
    }
    float periodX = prRectWidth(tile) + maxF32(tiles.spacing.x, 0);
    float periodY = prRectHeight(tile) + maxF32(tiles.spacing.y, 0);
    auto repeats = [](PrDrawExtend mode) { return mode == PrDrawExtend_Repeat || mode == PrDrawExtend_Reflect; };
    float firstX = repeats(tiles.x) ? floorF32((reach.left - tile.left) / periodX) : 0;
    float lastX = repeats(tiles.x) ? floorF32((reach.right - tile.left) / periodX) : 0;
    float firstY = repeats(tiles.y) ? floorF32((reach.top - tile.top) / periodY) : 0;
    float lastY = repeats(tiles.y) ? floorF32((reach.bottom - tile.top) / periodY) : 0;
    if (!prIRectIsEmpty(box) && (lastX - firstX + 1) * (lastY - firstY + 1) <= float(kListMaxTiles)) {
        for (float iy = firstY; iy <= lastY; iy += 1) {
            for (float ix = firstX; ix <= lastX; ix += 1) {
                PrRect at{
                    tile.left + ix * periodX, tile.top + iy * periodY, tile.right + ix * periodX,
                    tile.bottom + iy * periodY,
                };
                bool flipX = tiles.x == PrDrawExtend_Reflect && fmodF32(absF32(ix), 2) == 1;
                bool flipY = tiles.y == PrDrawExtend_Reflect && fmodF32(absF32(iy), 2) == 1;
                PrRect source{
                    flipX ? area.right : area.left, flipY ? area.bottom : area.top,
                    flipX ? area.left : area.right, flipY ? area.top : area.bottom,
                };
                PrDrawCommand tileClip{};
                tileClip.kind = PrDrawCommandKind_PushClipRect;
                tileClip.rect = at;
                pushClip(w, tileClip);
                PrDrawCommand place{};
                place.kind = PrDrawCommandKind_PushTransformAffine2;
                place.affine = prDAffine2FromAffine2(prAffine2Concat(
                    prAffine2Concat(prAffine2Translate(PrVec2{-source.left, -source.top}), prAffine2Scale(PrVec2{
                        prRectWidth(at) / prRectWidth(source),
                        prRectHeight(at) / prRectHeight(source),
                    })), prAffine2Translate(PrVec2{at.left, at.top})));
                w.transforms.push(prDrawTransformPush(&w.transforms.last(), &place));
                w.opacities.push(opacityOf(w) * clampF32(prFloat16ToFloat(c.color.a), 0, 1));
                if (!prIRectIsEmpty(clipOf(w)))
                    runCommands(w, prDrawListCommands(brush.image.source.list),
                        prDrawListCommandCount(brush.image.source.list));
                w.opacities.pop();
                w.transforms.pop();
                popClip(w);
            }
        }
    }
    w.transforms.pop();
    popClip(w);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

// Returns true if the operator draws as source-over. The other operators change what lies beneath in ways that blending
// over it cannot, and draw nothing.
bool drawsOver(PrDrawComposite composite) {
    switch (composite) {
        case PrDrawComposite_SourceOver:
        case PrDrawComposite_Copy:
        case PrDrawComposite_SourceAtop:
        case PrDrawComposite_PlusLighter:
        case PrDrawComposite_PlusDarker:
            return true;
        default:
            return false;
    }
}

// Measures the centerline of a stroke whose brush is mapped along or across it, and stores it in `w.map`. Returns false
// if the brush maps nowhere.
bool mapStroke(Walk& w, const PrDrawCommand& c) {
    Triangles* t = w.t;
    StrokeMap& m = w.map;
    const PrDrawTransform& x = w.transforms.last();
    const PrDrawStroke& stroke = *c.stroke;
    m.segments.clear();
    m.mapping = stroke.mapping;
    if (!prAffine2Invert(c.brush->transform, &m.unitToBrush))
        return false;
    prPathClear(t->shape);
    prDrawShapePath(&c, t->shape);
    prPathPolylineClear(&t->centerline);
    prPathFlatten(t->shape, x.affine, kTolerance, PrPathFlattening_Inside, &t->centerline);
    float scale = stroke.nonScaling ? 1 : prAffine2MaxScale(x.affine);
    // A stroke narrower than a pixel is drawn a pixel wide.
    m.reach = prDrawStrokeReach(&stroke) * maxF32(stroke.shape.width * scale, 1) + kFringe;
    const PrPathPolyline& centerline = t->centerline;
    for (uint32_t contour = 0; contour < centerline.contourCount; ++contour) {
        const PrVec2* points = centerline.points + centerline.contours[contour];
        uint32_t end = prPathPolylineContourEnd(&centerline, contour);
        uint32_t count = end - centerline.contours[contour];
        bool closed = prPathPolylineContourIsClosed(&centerline, contour);
        // A closed contour's last segment returns to its start.
        uint32_t segments = closed ? count : count - 1;
        float length = 0;
        for (uint32_t i = 0; i < segments; ++i) {
            PrVec2 a = points[i];
            PrVec2 b = points[(i + 1) % count];
            length += sqrtF32((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        }
        if (count < 2 || !(length > 0))
            continue;
        uint32_t first = m.segments.count;
        float along = 0;
        for (uint32_t i = 0; i < segments; ++i) {
            PrVec2 a = points[i];
            PrVec2 b = points[(i + 1) % count];
            float dx = b.x - a.x;
            float dy = b.y - a.y;
            float span = sqrtF32(dx * dx + dy * dy);
            if (!(span > 0))
                continue;
            MapSegment s{};
            s.a = a;
            s.b = b;
            s.direction = PrVec2{dx / span, dy / span};
            s.along = along;
            s.contourLength = length;
            PrPathStrokeWidth width = prPathStrokeWidthAt(&stroke.shape, (along + span / 2) / length);
            s.left = width.left * scale;
            s.right = width.right * scale;
            s.bounds = PrRect{
                minF32(a.x, b.x) - m.reach, minF32(a.y, b.y) - m.reach, maxF32(a.x, b.x) + m.reach,
                maxF32(a.y, b.y) + m.reach,
            };
            m.segments.push(s);
            along += span;
        }
        // Neighboring segments divide the plane along the bisector of their turn. An open contour's ends are unbounded.
        uint32_t n = m.segments.count - first;
        for (uint32_t i = 0; i < n; ++i) {
            if (i + 1 == n && !closed)
                break;
            MapSegment& s = m.segments.data[first + i];
            MapSegment& next = m.segments.data[first + (i + 1) % n];
            PrVec2 sum{s.direction.x + next.direction.x, s.direction.y + next.direction.y};
            PrVec2 normal = prVec2Normalize(sum);
            if (normal.x == 0 && normal.y == 0)
                normal = next.direction;
            s.endBound = true;
            s.endNormal = normal;
            next.startBound = true;
            next.startNormal = normal;
            // The bisector can extend past the box at the end by the stroke's reach along the bisector.
            s.bounds = prRectUnion(s.bounds, next.bounds);
        }
    }
    return true;
}

// Returns true if the command strokes with a brush mapped along or across the stroke.
bool mapsStroke(const PrDrawCommand& c) {
    return c.stroke != nullptr && c.brush != nullptr && c.stroke->mapping != PrDrawStrokeMapping_Plain;
}

void drawShape(Walk& w, const PrDrawCommand& c) {
    if (c.kind == PrDrawCommandKind_Line && c.stroke == nullptr)
        return;
    if (c.brush != nullptr && c.brush->kind == PrDrawBrushKind_Image &&
            c.brush->image.source.kind == PrDrawSourceKind_List && c.brush->image.source.list != nullptr) {
        if (c.stroke == nullptr)
            drawListBrush(w, c);
        return;
    }
    PrRect bounds = prDrawFillBounds(&c);
    Paint paint{};
    if (!makePaint(w, c.brush, c.color, bounds, &paint))
        return;
    if (mapsStroke(c) && !mapStroke(w, c))
        return;
    w.mapped = mapsStroke(c) && w.map.segments.count != 0;
    prPathClear(w.t->shape);
    prDrawShapePath(&c, w.t->shape);
    if (c.stroke != nullptr)
        strokePath(w, w.t->shape, c.stroke, c.antialias, paint);
    else
        fillPath(w, w.t->shape, c.rule, c.antialias, paint);
    w.mapped = false;
}

struct BorderDraw {
    Walk* w;
    const PrDrawCommand* command;
};

void drawBorderPiece(void* PR_NULLABLE context, const PrDrawBorderPiece* piece, const PrPathPolyline* shape,
        const PrPathPolyline* PR_NULLABLE cut) {
    BorderDraw& d = *static_cast<BorderDraw*>(context);
    Walk& w = *d.w;
    const PrDrawCommand& c = *d.command;
    PrRect bounds = prDrawFillBounds(&c);
    Paint paint{};
    if (!makePaint(w, c.border->sides[piece->side].brush, piece->color, bounds, &paint))
        return;
    const PrDrawTransform& x = w.transforms.last();
    prPathPolylineClear(&w.t->polyline);
    prPathPolylineAddPolyline(&w.t->polyline, shape, x.affine);
    if (cut == nullptr) {
        fillDevice(w, w.t->polyline, piece->rule, c.antialias, paint);
        return;
    }
    // The piece and its cut combine in target pixels.
    prPathPolylineClear(&w.t->cut);
    prPathPolylineAddPolyline(&w.t->cut, cut, x.affine);
    prPathPolylineClear(&w.t->combined);
    prPathPolylineCombine(&w.t->polyline, piece->rule, &w.t->cut, PrPathFillRule_NonZero, PrPathPolylineOp_Intersect,
        &w.t->combined);
    fillDevice(w, w.t->combined, PrPathFillRule_NonZero, c.antialias, paint);
}

void drawBorder(Walk& w, const PrDrawCommand& c) {
    float scale = prAffine2MaxScale(w.transforms.last().affine);
    if (!(scale > 0))
        return;
    BorderDraw d{&w, &c};
    prDrawBorderLower(c.border, kTolerance / scale, drawBorderPiece, &d);
}

void drawDecoration(Walk& w, const PrDrawCommand& c) {
    PrRect bounds = prDrawFillBounds(&c);
    Paint paint{};
    if (!makePaint(w, c.brush, c.color, bounds, &paint))
        return;
    const PrDrawTransform& x = w.transforms.last();
    float scale = prAffine2MaxScale(x.affine);
    if (!(scale > 0))
        return;
    prPathPolylineClear(&w.t->cut);
    prDrawDecorationOutline(c.decoration, kTolerance / scale, &w.t->cut);
    prPathPolylineClear(&w.t->polyline);
    prPathPolylineAddPolyline(&w.t->polyline, &w.t->cut, x.affine);
    fillDevice(w, w.t->polyline, PrPathFillRule_NonZero, c.antialias, paint);
}

void drawClear(Walk& w, const PrDrawCommand& c) {
    const PrIRect& clip = clipOf(w);
    if (prIRectIsEmpty(clip))
        return;
    Paint paint{};
    if (!makePaint(w, nullptr, c.color, PrRect{}, &paint))
        return;
    prPathClear(w.t->shape);
    prPathAddRect(w.t->shape, PrRect{float(clip.left), float(clip.top), float(clip.right), float(clip.bottom)});
    prPathPolylineClear(&w.t->polyline);
    prPathFlatten(w.t->shape, prAffine2Identity(), kTolerance, PrPathFlattening_Inside, &w.t->polyline);
    fillDevice(w, w.t->polyline, PrPathFillRule_NonZero, false, paint);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shadows

// The number of points along each corner of a shadow's outline.
constexpr uint32_t kShadowCornerPoints = 8;

// Appends the outline of the rectangle with fitted corners, with kShadowCornerPoints points along each corner,
// clockwise from the top left. A square corner repeats its point.
void cornerOutline(PrRect r, const PrCorner corners[4], PrAffine2 transform, Array<PrVec2>& out) {
    struct Frame {
        PrVec2 start;
        PrVec2 before;
        PrVec2 after;
        float radiusBefore;
        float radiusAfter;
    };
    const PrCorner& tl = corners[PrRectCorner_TopLeft];
    const PrCorner& tr = corners[PrRectCorner_TopRight];
    const PrCorner& br = corners[PrRectCorner_BottomRight];
    const PrCorner& bl = corners[PrRectCorner_BottomLeft];
    Frame frames[4] = {
        {{r.left, r.top + tl.radii.y}, {0, -1}, {1, 0}, tl.radii.y, tl.radii.x},
        {{r.right - tr.radii.x, r.top}, {1, 0}, {0, 1}, tr.radii.x, tr.radii.y},
        {{r.right, r.bottom - br.radii.y}, {0, 1}, {-1, 0}, br.radii.y, br.radii.x},
        {{r.left + bl.radii.x, r.bottom}, {-1, 0}, {0, -1}, bl.radii.x, bl.radii.y},
    };
    for (uint32_t k = 0; k < 4; ++k) {
        const Frame& f = frames[k];
        bool square = prCornerIsSquare(corners[k]);
        for (uint32_t i = 0; i < kShadowCornerPoints; ++i) {
            // A square corner is its corner point, which lies along the side before it by its whole radius.
            PrVec2 uv;
            if (square)
                uv = PrVec2{1, 0};
            else
                uv = prCornerCurvePoint(corners[k].shape, float(i) / float(kShadowCornerPoints - 1));
            float along = f.radiusBefore * uv.x;
            float inward = f.radiusAfter * uv.y;
            PrVec2 p{
                f.start.x + f.before.x * along + f.after.x * inward,
                f.start.y + f.before.y * along + f.after.y * inward,
            };
            out.push(prAffine2MapPoint(transform, p));
        }
    }
}

// Draws the triangle cut to the convex outline. The outline's inside is on the side where `sign` times the turn is
// positive.
void drawCut(Walk& w, const Piece triangle[3], const Array<PrVec2>& outline, float sign, const Paint& paint) {
    Piece polygon[kMaxPiece];
    memCopy(polygon, triangle, sizeof(Piece) * 3);
    uint32_t count = keepInside(polygon, 3, outline.data, outline.count, sign);
    if (count < 3)
        return;
    useDraw(w, paint.texture, paint.sampler, clipOf(w));
    drawPolygon(w, polygon, count, paint);
}

// Draws a shadow. A drop shadow is its shape with a band kShadowBand sigmas wide across the edge, with the coverage
// falling from inside to outside. An inner shadow is the reverse, cut to the opening. A shape narrower than the band
// has its peak lowered. Neither is cut by the caster.
void drawShadow(Walk& w, const PrDrawCommand& c) {
    Triangles* t = w.t;
    if (prIRectIsEmpty(clipOf(w)))
        return;
    const PrDrawShadowKind kind =
        c.kind == PrDrawCommandKind_InnerShadow ? PrDrawShadowKind_Inner : PrDrawShadowKind_Drop;
    PrDrawShadowGeometry g = prDrawShadowGeometry(c.shadow, kind);
    PrAffine2 device = w.transforms.last().affine;
    float scale = prAffine2MaxScale(device);
    if (!(scale > 0))
        return;
    PrAffine2 shifted = device;
    if (c.shadow->nonScaling)
        shifted = prAffine2Concat(device, prAffine2Translate(c.shadow->offset));
    float sigma = c.shadow->nonScaling ? g.sigma : g.sigma * scale;
    float half = maxF32(sigma * kShadowBand / 2, kFringe / 2) / scale;
    PrRect bounds = prDrawFillBounds(&c);
    Paint paint{};
    if (!makePaint(w, c.brush, c.color, bounds, &paint))
        return;

    float width = prRectWidth(g.shape.rect);
    float height = prRectHeight(g.shape.rect);
    float insetX = minF32(half, width / 2);
    float insetY = minF32(half, height / 2);
    float peak = minF32(1, width / (2 * half)) * minF32(1, height / (2 * half));
    float insets[4] = {insetY, insetX, insetY, insetX};
    float outsets[4] = {-half, -half, -half, -half};
    PrCornerRect innerShape = prCornerRectInset(g.shape, insets);
    PrCornerRect outerShape = prCornerRectInset(g.shape, outsets);
    PrRect& innerRect = innerShape.rect;
    if (prRectIsEmpty(innerRect)) {
        PrVec2 middle = prRectCenter(g.shape.rect);
        innerRect = PrRect{middle.x, middle.y, middle.x, middle.y};
        for (PrCorner& corner : innerShape.corners)
            corner = PrCorner{{0, 0}, 1};
    }
    Array<PrVec2> ring;
    cornerOutline(innerRect, innerShape.corners, shifted, ring);
    uint32_t n = ring.count;
    cornerOutline(outerShape.rect, outerShape.corners, shifted, ring);
    float innerCoverage = kind == PrDrawShadowKind_Inner ? 0 : peak;
    float outerCoverage = kind == PrDrawShadowKind_Inner ? 1 : 0;

    if (kind != PrDrawShadowKind_Inner) {
        // A fan from the middle over the inner outline, and the band from the inner outline out to the outer one.
        Array<PrVec2> points;
        Array<float> coverage;
        Array<uint32_t> indices;
        points.push(prAffine2MapPoint(shifted, prRectCenter(innerRect)));
        coverage.push(peak);
        for (uint32_t i = 0; i < ring.count; ++i) {
            points.push(ring.data[i]);
            coverage.push(i < n ? innerCoverage : outerCoverage);
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t a = 1 + i;
            uint32_t b = 1 + (i + 1 < n ? i + 1 : 0);
            uint32_t triangles[9] = {0, a, b, a, b, b + n, a, b + n, a + n};
            indices.append(triangles, 9);
        }
        drawMesh(w, PrPathMesh{points.data, coverage.data, points.count, indices.data, indices.count}, paint);
        points.reset();
        coverage.reset();
        indices.reset();
        ring.reset();
        return;
    }

    // The opening's outline, which cuts everything else.
    Array<PrVec2> opening;
    PrCornerRect fitted = prCornerRectFit(c.shadow->shape);
    cornerOutline(fitted.rect, fitted.corners, device, opening);
    double area = 0;
    for (uint32_t i = 0; i < opening.count; ++i) {
        PrVec2 a = opening.data[i];
        PrVec2 b = opening.data[i + 1 < opening.count ? i + 1 : 0];
        area += double(a.x) * double(b.y) - double(b.x) * double(a.y);
    }
    float sign = area > 0 ? 1 : -1;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = i + 1 < n ? i + 1 : 0;
        Piece a{ring.data[i], innerCoverage, 0, PrVec2{0, 0}};
        Piece b{ring.data[j], innerCoverage, 0, PrVec2{0, 0}};
        Piece bo{ring.data[n + j], outerCoverage, 0, PrVec2{0, 0}};
        Piece ao{ring.data[n + i], outerCoverage, 0, PrVec2{0, 0}};
        Piece first[3] = {a, b, bo};
        Piece second[3] = {a, bo, ao};
        drawCut(w, first, opening, sign, paint);
        drawCut(w, second, opening, sign, paint);
    }
    // Beyond the band, the opening's bounds outside the band's outer outline are drawn at full coverage.
    PrRect reach = prAffine2MapRect(device, c.shadow->shape.rect);
    prPathClear(t->region);
    prPathAddRect(t->region, PrRect{reach.left - 1, reach.top - 1, reach.right + 1, reach.bottom + 1});
    prPathMoveTo(t->region, ring.data[n]);
    for (uint32_t i = n + 1; i < ring.count; ++i)
        prPathLineTo(t->region, ring.data[i]);
    prPathClose(t->region);
    prPathPolylineClear(&t->polyline);
    prPathFlatten(t->region, prAffine2Identity(), kTolerance, PrPathFlattening_Balanced, &t->polyline);
    PrPathMesh mesh = prPathTessellatorFill(t->tessellator, &t->polyline, PrPathFillRule_EvenOdd, 0);
    for (uint32_t i = 0; i + 2 < mesh.indexCount; i += 3) {
        Piece triangle[3] = {
            {mesh.points[mesh.indices[i]], 1, 0, PrVec2{0, 0}},
            {mesh.points[mesh.indices[i + 1]], 1, 0, PrVec2{0, 0}},
            {mesh.points[mesh.indices[i + 2]], 1, 0, PrVec2{0, 0}},
        };
        drawCut(w, triangle, opening, sign, paint);
    }
    opening.reset();
    ring.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Draws the image's texels from each source rectangle onto its destination, colored. The destination's edges are
// antialiased if `antialias`. A list source plays its commands instead, with only the color's alpha.
void drawImageRects(Walk& w, const PrDrawCommand& c, const PrDrawImageRect* rects, uint32_t count, bool antialias) {
    const PrDrawSource& source = c.nineSlice.source;
    if (prIRectIsEmpty(clipOf(w)) || !(c.color.a > 0))
        return;
    if (!sampled(source)) {
        for (uint32_t i = 0; source.kind == PrDrawSourceKind_List && source.list != nullptr && i < count; ++i) {
            const PrRect& s = rects[i].source;
            PrRect area{
                source.rect.left + s.left, source.rect.top + s.top, source.rect.left + s.right,
                source.rect.top + s.bottom,
            };
            drawListRect(w, source.list, area, rects[i].destination, prFloat16ToFloat(c.color.a));
        }
        return;
    }
    DrawTexture texture = textureOfSource(w, source);
    if (!texture.exists())
        return;
    // The paint's brush space is the image's texels.
    const PrDrawTransform& x = w.transforms.last();
    Paint paint{};
    paint.kind = PaintKind_Image;
    paint.color = c.color;
    paint.alpha = opacityOf(w);
    paint.texture = texture;
    bool nearest = (c.sampling & PrDrawSamplingFlag_Kind) == PrDrawSampling_Nearest;
    paint.sampler = nearest ? TrianglesSampler_NearestClamp : TrianglesSampler_LinearClamp;
    paint.cut = PaintCut_None;
    paint.clampUv = true;
    PrAffine2 fromDevice = prAffine2Identity();
    if (!prAffine2Invert(x.affine, &fromDevice))
        return;
    PrVec2 size = sourceSize(source);
    paint.uvFromBrush = prAffine2Scale(PrVec2{1 / size.x, 1 / size.y});
    for (uint32_t i = 0; i < count; ++i) {
        const PrRect& d = rects[i].destination;
        if (!(d.right > d.left && d.bottom > d.top))
            continue;
        PrRect s = rects[i].source;
        s = PrRect{
            source.rect.left + s.left, source.rect.top + s.top, source.rect.left + s.right,
            source.rect.top + s.bottom,
        };
        PrAffine2 toSource = prAffine2Concat(prAffine2Translate(PrVec2{-d.left, -d.top}),
            prAffine2Scale(PrVec2{prRectWidth(s) / prRectWidth(d), prRectHeight(s) / prRectHeight(d)}));
        toSource = prAffine2Concat(toSource, prAffine2Translate(PrVec2{s.left, s.top}));
        paint.fromDevice = prAffine2Concat(fromDevice, toSource);
        paint.uvBounds = PrRect{s.left / size.x, s.top / size.y, s.right / size.x, s.bottom / size.y};
        prPathClear(w.t->shape);
        prPathAddRect(w.t->shape, d);
        fillPath(w, w.t->shape, PrPathFillRule_NonZero, antialias, paint);
    }
}

// Draws a nine-slice. Its pieces meet without fringes, so that no seams show between them.
void drawNineSlice(Walk& w, const PrDrawCommand& c) {
    const PrRect& area = c.nineSlice.source.rect;
    uint32_t count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{prRectWidth(area), prRectHeight(area)},
        c.nineSlice.destination, nullptr, 0);
    Array<PrDrawImageRect> rects;
    rects.reserve(count);
    rects.count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{prRectWidth(area), prRectHeight(area)},
        c.nineSlice.destination, rects.data, count);
    drawImageRects(w, c, rects.data, rects.count, false);
    rects.reset();
}

// Draws the image's texels at a device point, over the target regardless of the clip. The texels are blended instead of
// replacing the target's pixels.
void writePixels(Walk& w, const PrDrawCommand& c) {
    const PrDrawImage* image = c.writePixels.image;
    uint32_t texture = textureOfImage(w, image);
    if (texture == 0 || prIRectIsEmpty(w.target))
        return;
    float x0 = float(c.writePixels.origin.x);
    float y0 = float(c.writePixels.origin.y);
    float x1 = x0 + float(prDrawImageExtent(image).x);
    float y1 = y0 + float(prDrawImageExtent(image).y);
    useDraw(w, DrawTexture{texture, 0}, TrianglesSampler_NearestClamp, w.target);
    uint32_t base = w.b->vertices.count;
    uint32_t white = 0xffffffffu;
    w.b->vertices.push(TrianglesVertex{{x0, y0}, {0, 0}, white});
    w.b->vertices.push(TrianglesVertex{{x1, y0}, {1, 0}, white});
    w.b->vertices.push(TrianglesVertex{{x1, y1}, {1, 1}, white});
    w.b->vertices.push(TrianglesVertex{{x0, y1}, {0, 1}, white});
    uint32_t indices[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    addIndices(w, indices, 6);
}

// ---------------------------------------------------------------------------------------------------------------------
// Glyphs

// Draws the glyph from the atlas if it is drawn without rotation or skew at a size that the atlas holds. Returns false
// if the glyph must be filled as a path instead.
bool drawAtlasGlyph(Walk& w, const PrDrawCommand& c, uint32_t index, const Paint& paint) {
    const PrFontGlyphRun& run = *c.glyphs;
    const PrDrawTransform& x = w.transforms.last();
    if (!c.antialias)
        return false;
    PrDrawAtlasEntry g{};
    if (!prDrawAtlasGlyph(w.t->atlas, &run, index, x.affine, &g))
        return false;
    if (g.extent.x == 0)
        return true;
    float size = float(w.t->atlasSize);
    float left = float(g.origin.x);
    float top = float(g.origin.y);
    float right = left + float(g.extent.x);
    float bottom = top + float(g.extent.y);
    PrVec2 corners[4] = {{left, top}, {right, top}, {right, bottom}, {left, bottom}};
    PrVec2 uvs[4] = {
        {float(g.texel.x) / size, float(g.texel.y) / size},
        {float(g.texel.x + g.extent.x) / size, float(g.texel.y) / size},
        {float(g.texel.x + g.extent.x) / size, float(g.texel.y + g.extent.y) / size},
        {float(g.texel.x) / size, float(g.texel.y + g.extent.y) / size},
    };
    useDraw(w, DrawTexture{pageTexture(w.t, g.page), 0}, TrianglesSampler_LinearClamp, clipOf(w));
    if (outlined(w)) {
        Paint quad = paint;
        quad.kind = paint.kind == PaintKind_Image ? PaintKind_Solid : paint.kind;
        quad.pieceUv = true;
        quad.clipTile = false;
        Piece polygon[kMaxPiece];
        for (uint32_t k = 0; k < 4; ++k)
            polygon[k] = Piece{corners[k], 1, 0, uvs[k]};
        drawPolygon(w, polygon, 4, quad);
        return true;
    }
    uint32_t base = w.b->vertices.count;
    for (uint32_t k = 0; k < 4; ++k)
        w.b->vertices.push(TrianglesVertex{corners[k], uvs[k], colorAt(paint, corners[k], 1)});
    uint32_t indices[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    addIndices(w, indices, 6);
    return true;
}

// Draws a glyph run. Each glyph draws on its own. A filled color glyph plays its list. Other glyphs come from the
// atlas, or are filled or stroked as paths.
void drawGlyphs(Walk& w, const PrDrawCommand& c) {
    const PrFontGlyphRun& run = *c.glyphs;
    PrRect bounds = prDrawFillBounds(&c);
    Paint paint{};
    if (!makePaint(w, c.brush, c.color, bounds, &paint))
        return;
    if (mapsStroke(c) && !mapStroke(w, c))
        return;
    PrColor foreground = c.color;
    foreground.a = 1;
    for (uint32_t i = 0; i < run.glyphCount; ++i) {
        if (c.stroke == nullptr) {
            float pixelsPerEm = prDrawGlyphPixelsPerEm(&run, i, w.transforms.last().affine);
            PrDrawList* drawn =
                prDrawGlyphCacheList(w.t->lists, run.font, run.glyphs[i].id, pixelsPerEm, &run.palette, foreground);
            if (drawn != nullptr) {
                PrDrawGroup group{};
                PrDrawCommand commands[kPrDrawGlyphMaxCommands];
                runCommands(w, commands, prDrawGlyphListCommands(&c, i, drawn, &group, commands));
                continue;
            }
            if (drawAtlasGlyph(w, c, i, paint))
                continue;
        }
        const PrDrawTransform& x = w.transforms.last();
        prPathClear(w.t->shape);
        if (!prDrawGlyphPath(&run, i, &x.affine, w.t->shape))
            continue;
        w.mapped = mapsStroke(c) && w.map.segments.count != 0;
        if (c.stroke != nullptr)
            strokePath(w, w.t->shape, c.stroke, c.antialias, paint);
        else
            fillPath(w, w.t->shape, PrPathFillRule_NonZero, c.antialias, paint);
        w.mapped = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

// Appends the outline of a path in target pixels to `out` as one convex contour. Returns a sign such that the inside is
// on the side where the sign times the turn is positive. Returns 0 if the outline is not one convex contour.
float convexOutline(const PrPathPolyline& device, Array<PrVec2>& out) {
    if (device.contourCount != 1)
        return 0;
    const PrVec2* p = device.points;
    uint32_t n = device.pointCount;
    if (n > 1 && p[n - 1].x == p[0].x && p[n - 1].y == p[0].y)
        --n;
    // The contour is convex and simple if every turn has one sign, and the direction's x and y each change sign at most
    // twice. A star whose turns all agree fails the second test.
    int turn = 0;
    int lastX = 0;
    int lastY = 0;
    uint32_t flipsX = 0;
    uint32_t flipsY = 0;
    bool convex = n >= 3;
    for (uint32_t i = 0; i < n && convex; ++i) {
        PrVec2 a = p[i];
        PrVec2 b = p[(i + 1) % n];
        PrVec2 c = p[(i + 2) % n];
        float cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
        convex = s == 0 || turn == 0 || s == turn;
        turn = s != 0 ? s : turn;
        int dx = b.x > a.x ? 1 : b.x < a.x ? -1 : 0;
        int dy = b.y > a.y ? 1 : b.y < a.y ? -1 : 0;
        flipsX += dx != 0 && lastX != 0 && dx != lastX ? 1 : 0;
        flipsY += dy != 0 && lastY != 0 && dy != lastY ? 1 : 0;
        lastX = dx != 0 ? dx : lastX;
        lastY = dy != 0 ? dy : lastY;
    }
    if (!convex || turn == 0 || flipsX > 2 || flipsY > 2)
        return 0;
    out.append(p, n);
    return float(turn);
}

// Pushes a clip. Its device bounds become a scissor rectangle. If the clip is not a rectangle that stays axis-aligned,
// and its outline is convex, the outline also cuts what draws within the clip.
void pushClip(Walk& w, const PrDrawCommand& c) {
    Clip clip = w.clips.last();
    const PrDrawTransform& x = w.transforms.last();
    PrRect rect{};
    bool outline = true;
    if (c.kind == PrDrawCommandKind_PushClipTargetRect) {
        rect = c.rect;
        outline = false;
    } else {
        prPathClear(w.t->shape);
        prDrawShapePath(&c, w.t->shape);
        prPathClear(w.t->mapped);
        prPathAddPath(w.t->mapped, w.t->shape, x.affine);
        rect = prPathBounds(w.t->mapped);
        outline = !(c.kind == PrDrawCommandKind_PushClipRect && prAffine2IsAxisAligned(x.affine));
        prPathPolylineClear(&w.t->outline);
        prPathFlatten(w.t->shape, x.affine, kClipTolerance, PrPathFlattening_Inside, &w.t->outline);
    }
    if (prDrawTransformDrawsNothing(&x, &c) || prRectIsEmpty(rect)) {
        clip.box = PrIRect{0, 0, 0, 0};
    } else {
        PrIRect box{
            saturateI32(double(roundF32(rect.left))), saturateI32(double(roundF32(rect.top))),
            saturateI32(double(roundF32(rect.right))), saturateI32(double(roundF32(rect.bottom))),
        };
        clip.box = prIRectIntersect(clip.box, box);
        if (outline && !prIRectIsEmpty(clip.box)) {
            uint32_t start = w.clipPoints.count;
            float sign = convexOutline(w.t->outline, w.clipPoints);
            if (sign != 0) {
                w.outlines.count = clip.outlines;
                w.outlines.push(ClipOutline{start, w.clipPoints.count - start, sign});
                ++clip.outlines;
            }
        }
    }
    w.clips.push(clip);
}

// Pops a clip, and the outline that it pushed.
void popClip(Walk& w) {
    w.clips.pop();
    uint32_t outlines = w.clips.last().outlines;
    w.outlines.count = outlines;
    w.clipPoints.count = outlines == 0 ? 0 : w.outlines.data[outlines - 1].start + w.outlines.data[outlines - 1].count;
}

void runCommands(Walk& w, const PrDrawCommand* commands, uint32_t count) {
    PrDrawSnapped snapped;
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = *prDrawTransformSnap(&w.transforms.last(), &commands[i], &snapped);
        // A projected group needs a layer, which triangles does not have, so the group and its contents draw nothing.
        if (c.kind == PrDrawCommandKind_PushGroup && c.group->projection != nullptr) {
            i = prDrawCommandGroupEnd(commands, count, i);
            continue;
        }
        if (prDrawTransformDrawsNothing(&w.transforms.last(), &c)) {
            // A push still opens a scope that its pop closes.
            if (c.kind >= PrDrawCommandKind_PushClipRect && c.kind <= PrDrawCommandKind_PushClipPath)
                w.clips.push(Clip{PrIRect{0, 0, 0, 0}, w.clips.last().outlines});
            else if (c.kind == PrDrawCommandKind_PushGroup)
                w.opacities.push(0);
            continue;
        }
        bool painting = c.kind <= PrDrawCommandKind_Glyphs && c.kind != PrDrawCommandKind_WritePixels &&
            c.kind != PrDrawCommandKind_Clear;
        if (painting && !drawsOver(c.composite))
            continue;
        switch (c.kind) {
            case PrDrawCommandKind_Clear:
                drawClear(w, c);
                break;
            case PrDrawCommandKind_Border:
                drawBorder(w, c);
                break;
            case PrDrawCommandKind_DropShadow:
            case PrDrawCommandKind_InnerShadow:
                drawShadow(w, c);
                break;
            case PrDrawCommandKind_Decoration:
                drawDecoration(w, c);
                break;
            case PrDrawCommandKind_NineSlice:
                drawNineSlice(w, c);
                break;
            case PrDrawCommandKind_WritePixels:
                writePixels(w, c);
                break;
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
                popClip(w);
                break;
            case PrDrawCommandKind_PushGroup:
                w.opacities.push(opacityOf(w) * clampF32(c.group->opacity, 0, 1));
                break;
            case PrDrawCommandKind_PopGroup:
                w.opacities.pop();
                break;
            default:
                drawShape(w, c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Builds

// Adds the requests for the atlas pages that changed, and for the images that went unused too long.
void finishRequests(Walk& w) {
    Triangles* t = w.t;
    uint32_t size = t->atlasSize;
    for (uint32_t i = 0; i < prDrawAtlasPageCount(t->atlas); ++i) {
        uint32_t texture = pageTexture(t, i);
        PrIRect region{};
        bool changed = prDrawAtlasTakeChanges(t->atlas, i, &region);
        bool create = i >= t->pagesCreated;
        if (create)
            region = PrIRect{0, 0, int32_t(size), int32_t(size)};
        else if (!changed)
            continue;
        uint32_t left = uint32_t(region.left);
        uint32_t top = uint32_t(region.top);
        uint32_t width = prIRectWidth(region);
        uint32_t height = prIRectHeight(region);
        // The coverage, as white with that alpha.
        size_t offset = w.b->texels.count;
        size_t bytes = size_t(width) * height * 4;
        w.b->texels.reserve(uint32_t(offset + bytes));
        w.b->texels.count = uint32_t(offset + bytes);
        PrPixelView from = prDrawAtlasPageTexels(t->atlas, i);
        uint8_t* to = w.b->texels.data + offset;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = from.data + size_t(top + y) * from.stride + left;
            for (uint32_t x = 0; x < width; ++x, to += 4) {
                to[0] = to[1] = to[2] = 0xff;
                to[3] = row[x];
            }
        }
        w.b->pending.push(PendingTexels{w.b->requests.count, offset});
        TrianglesTextureOp op = create ? TrianglesTextureOp_Create : TrianglesTextureOp_Update;
        request(w, TrianglesTextureRequest{
            op, texture, {left, top},
            PrPixelView{nullptr, size_t(width) * 4, {width, height}},
        });
    }
    t->pagesCreated = prDrawAtlasPageCount(t->atlas);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < t->images.count; ++i) {
        const CachedImage& image = t->images.data[i];
        if (t->build - image.lastUsed < kIdleBuilds) {
            t->images.data[kept++] = image;
            continue;
        }
        request(w, TrianglesTextureRequest{TrianglesTextureOp_Destroy, image.texture, {0, 0}, {}});
    }
    t->images.count = kept;
    t->imageIndex.rebuild(kept, [&](uint32_t i) { return t->images.data[i].id; });
    // The texel storage has stopped growing.
    for (uint32_t i = 0; i < w.b->pending.count; ++i) {
        const PendingTexels& pending = w.b->pending.data[i];
        w.b->requests.data[pending.request].texels.data = w.b->texels.data + pending.offset;
    }
}

void describe(const FrameStorage* b, uint32_t width, uint32_t height, TrianglesFrame* out) {
    *out = TrianglesFrame{
        {width, height},
        b->requests.data,
        b->requests.count,
        b->vertices.data,
        b->vertices.count,
        b->indices.data,
        b->indices.count,
        b->draws.data,
        b->draws.count,
    };
}

void forgetPages(Triangles* t) {
    prDrawAtlasClear(t->atlas);
    t->pageTextures.clear();
    t->pagesCreated = 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building frames

// Creates the state that builds share, for the options.
Triangles* trianglesCreate(const PrDrawTrianglesOptions& o) {
    Triangles* t = allocZeroed<Triangles>();
    t->atlasSize = o.atlasSize < 64 ? 64 : o.atlasSize;
    PrDrawAtlasOptions atlas = prDrawAtlasOptionsDefault();
    atlas.size = t->atlasSize;
    atlas.solidSize = kWhiteSize;
    t->atlas = prDrawAtlasCreate(&atlas);
    t->lists = prDrawGlyphCacheCreate(o.glyphDecoder);
    t->tessellator = prPathTessellatorCreate();
    t->shape = prPathCreate();
    t->mapped = prPathCreate();
    t->region = prPathCreate();
    t->brushShape = prPathCreate();
    return t;
}

void trianglesDestroy(Triangles* t) {
    prDrawAtlasDestroy(t->atlas);
    t->pageTextures.reset();
    t->images.reset();
    t->imageIndex.reset();
    for (uint32_t i = 0; i < t->ramps.count; ++i)
        prMemoryFree(t->ramps.data[i]);
    t->ramps.reset();
    t->rampIndex.reset();
    prDrawGlyphCacheDestroy(t->lists);
    prPathTessellatorDestroy(t->tessellator);
    prPathPolylineReset(&t->polyline);
    prPathDestroy(t->shape);
    prPathDestroy(t->mapped);
    prPathDestroy(t->region);
    PrPathPolyline* polylines[] = {&t->fill, &t->aligned, &t->strokeRegion, &t->combined, &t->cut, &t->outline};
    for (PrPathPolyline* polyline : polylines)
        prPathPolylineReset(polyline);
    prPathPolylineReset(&t->centerline);
    prPathDestroy(t->brushShape);
    prMemoryFree(t);
}

// Forgets every texture, for example after the host loses its device. The next build recreates the textures that it
// draws with.
void trianglesForgetResources(Triangles* t) {
    forgetPages(t);
    t->images.clear();
    t->imageIndex.index.clear();
}

void storageDestroy(FrameStorage* b) {
    b->vertices.reset();
    b->indices.reset();
    b->draws.reset();
    b->requests.reset();
    b->texels.reset();
    b->pending.reset();
    prMemoryFree(b);
}

// Builds the frame that draws the list over a target `width` by `height` pixels into the storage, and describes it in
// `out`. The frame stays valid until the storage's next build. The requests' texels stay valid until the next build
// with any storage.
void trianglesBuild(Triangles* t, const PrDrawList* list, uint32_t width, uint32_t height, FrameStorage* b,
        TrianglesFrame* out) {
    b->vertices.clear();
    b->indices.clear();
    b->draws.clear();
    b->pathFills = 0;
    b->requests.clear();
    b->texels.clear();
    b->pending.clear();
    ++t->build;
    t->rampCount = 0;
    t->rampIndex.clear();
    prDrawAtlasNext(t->atlas);
    prDrawGlyphCacheBegin(t->lists);
    Walk w{};
    w.t = t;
    w.b = b;
    w.target = PrIRect{0, 0, int32_t(width), int32_t(height)};
    w.transforms.push(prDrawTransformFromAffine2(prAffine2Identity()));
    w.clips.push(Clip{w.target, 0});
    w.opacities.push(1);
    runCommands(w, prDrawListCommands(list), prDrawListCommandCount(list));
    finishRequests(w);
    prDrawGlyphCacheEnd(t->lists);
    w.transforms.reset();
    w.clips.reset();
    w.outlines.reset();
    w.clipPoints.reset();
    w.opacities.reset();
    w.map.segments.reset();
    if (b->draws.count != 0 && b->draws.last().indexCount == 0)
        b->draws.pop();
    describe(b, width, height, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// The renderer

// The renderer: what builds its frames, and its state on its device.
struct TrianglesRenderer : PrDrawRenderer {
    TrianglesDevice* device;
    Triangles* triangles;
};

// A frame: the data that the build produced, and the storage that it points into.
struct TrianglesDrawFrame : PrDrawFrame {
    FrameStorage* storage;
    TrianglesFrame built;
};

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    TrianglesDrawFrame* frame = allocZeroed<TrianglesDrawFrame>();
    frame->renderer = renderer;
    frame->storage = allocZeroed<FrameStorage>();
    return frame;
}

void destroyFrame(PrDrawRenderer*, PrDrawFrame* handle) {
    TrianglesDrawFrame* frame = static_cast<TrianglesDrawFrame*>(handle);
    storageDestroy(frame->storage);
    prMemoryFree(frame);
}

PrStatus prepare(PrDrawRenderer* renderer, PrDrawFrame* handle, const PrDrawList* list, const PrDrawTarget* target) {
    TrianglesRenderer* r = static_cast<TrianglesRenderer*>(renderer);
    TrianglesDrawFrame* frame = static_cast<TrianglesDrawFrame*>(handle);
    trianglesBuild(r->triangles, list, target->extent.x, target->extent.y, frame->storage, &frame->built);
    trianglesDevicePrepare(r->device, &frame->built, target->commands);
    return PrStatus_Ok;
}

void render(PrDrawRenderer* renderer, PrDrawFrame* handle, PrGpuPass* pass, PrIVec2 origin) {
    TrianglesRenderer* r = static_cast<TrianglesRenderer*>(renderer);
    TrianglesDrawFrame* frame = static_cast<TrianglesDrawFrame*>(handle);
    trianglesDeviceRender(r->device, &frame->built, pass, origin);
}

void dropResources(PrDrawRenderer* renderer) {
    TrianglesRenderer* r = static_cast<TrianglesRenderer*>(renderer);
    trianglesForgetResources(r->triangles);
    trianglesDeviceDropResources(r->device);
}

void stats(PrDrawRenderer*, const PrDrawFrame* handle, PrDrawStats* out) {
    const TrianglesDrawFrame* frame = static_cast<const TrianglesDrawFrame*>(handle);
    const TrianglesFrame& f = frame->built;
    out->passes = 1;
    for (uint32_t i = 0; i < f.drawCount; ++i)
        out->draws += f.draws[i].indexCount != 0 ? 1 : 0;
    out->primitives = f.indexCount / 3;
    out->pathFills = frame->storage->pathFills;
    uint64_t bytes = uint64_t(f.vertexCount) * sizeof(TrianglesVertex) + uint64_t(f.indexCount) * 4;
    for (uint32_t i = 0; i < f.requestCount; ++i)
        bytes += uint64_t(f.requests[i].texels.extent.x) * f.requests[i].texels.extent.y * 4;
    out->uploadBytes = bytes;
}

void destroyRenderer(PrDrawRenderer* renderer) {
    TrianglesRenderer* r = static_cast<TrianglesRenderer*>(renderer);
    trianglesDestroy(r->triangles);
    trianglesDeviceDestroy(r->device);
    prMemoryFree(r);
}

TrianglesRenderer* createRenderer(TrianglesDevice* device, const PrDrawTrianglesOptions* PR_NULLABLE options) {
    TrianglesRenderer* r = allocZeroed<TrianglesRenderer>();
    r->device = device;
    r->triangles = trianglesCreate(options != nullptr ? *options : prDrawTrianglesOptionsDefault());
    r->destroy = destroyRenderer;
    r->frameCreate = createFrame;
    r->frameDestroy = destroyFrame;
    r->framePrepare = prepare;
    r->frameRender = render;
    r->frameStats = stats;
    r->dropResources = dropResources;
    return r;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawTrianglesOptions prDrawTrianglesOptionsDefault(void) {
    PrDrawTrianglesOptions options{};
    options.atlasSize = 1024;
    return options;
}

PrStatus prDrawTrianglesCreate(PrGpuDevice* device, const PrDrawTrianglesOptions* PR_NULLABLE options,
        PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    TrianglesDevice* state = nullptr;
    PrStatus status = trianglesDeviceCreate(device, &state);
    if (status != PrStatus_Ok)
        return status;
    *out = createRenderer(state, options);
    return PrStatus_Ok;
}
