// The fragment draw backend, which turns a list into instances whose coverage, clips and paint a fragment shader
// evaluates.
//
// A build walks the list in the same way as every backend. Each painting command becomes instances of its color, and a
// paint record if its brush or compositing needs one. A filled and antialiased rectangle, rounded rectangle or ellipse
// with round corners is one instance whose kind computes the shape's coverage. A shadow is one instance that blurs its
// shape and cuts out its caster. Glyphs and paths drawn without rotation or skew at modest sizes are instances of the
// atlas's coverage, which persists from build to build. Everything else becomes paths in target pixels, filled as the
// options' `paths` selects: triangles tessellated with a one-pixel fringe for their coverage, or instances of sparse
// coverage.
//
// The clips in force are one record: the intersection of the rectangle clips, the rounded or turned rectangles under a
// transform of the plane, and a mask layer that any other clip draws into.

#include "pr/draw_fragment.h"

#include "pr/alloc.h"
#include "pr/color_space.h"
#include "pr/draw_commands.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_projection.h"
#include "pr/draw_renderer_backend.h"
#include "pr/draw_shared_atlas.h"
#include "pr/draw_transform.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/path_measure.h"
#include "pr/path_polyline.h"
#include "pr/path_sparse.h"
#include "pr/path_tessellator.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "draw/backends/fragment/src/draw_fragment_device.h"
#include "draw/backends/fragment/src/draw_fragment_frame.h"
#include "draw/backends/shared/src/draw_shared_layer.h"

namespace pr_impl {

namespace {

// The largest distance of a flattened curve from the curve, in target pixels, for tessellated paths and for sparse
// paths. The segments of sparse paths cost no vertices.
constexpr float kTolerance = 0.25f;
constexpr float kSparseTolerance = 0.05f;

// The width of the band that antialiases a tessellated edge, in target pixels.
constexpr float kFringe = 1;

// The number of consecutive builds in which a texture goes unused before it is destroyed.
constexpr uint64_t kIdleBuilds = 8;

// How far a shadow's blur reaches, in sigmas.
constexpr float kShadowReach = 4;

// The number of blurred levels, including the unblurred input, that a progressive blur mixes between.
constexpr uint32_t kProgressiveLevels = 5;

// The tile sizes of sparse coverage: strips a few pixels high, whose coverage the builder computes, and square tiles,
// whose segments the shader integrates.
constexpr uint32_t kStripTile = 4;
constexpr uint32_t kSegmentTile = 16;

// The smallest cell of a mapped stroke's grid, in pixels, and the maximum number of cells along a side.
constexpr float kMapCell = 16;
constexpr float kMapCells = 64;

// ---------------------------------------------------------------------------------------------------------------------
// State

struct CachedImage {
    uint64_t id;
    bool mipmapped;
    uint32_t texture;
    uint64_t lastUsed;
};

// Returns an image's key among the cached images, made from its id and whether it has mip levels.
uint64_t imageKey(uint64_t id, bool mipmapped) {
    return id << 1 | (mipmapped ? 1 : 0);
}

// A gradient's ramp for a color, which this build wrote into its frame.
struct CachedRamp {
    const PrDrawBrush* brush;
    PrColor color;
    uint32_t index;             // the ramp's index in the frame
};

// The state of the walk that an embedded list's records depend on, beside the list itself.
struct PreparedKey {
    PrAffine2 transform;
    PrIRect clipBox;
    PrRect clipRect;
    float opacity;
    uint32_t layer;
    PrDrawComposite composite;
    PrDrawBlendMode blendMode;
    bool knockout;
};

// The records that a build made for an embedded list, kept for later builds that embed the same list in the same state,
// or in the same state moved by whole target pixels. Indices into the frame's arrays are local: values, paints and
// clips count from the list's first, and textures index `tables`.
struct PreparedList {
    uint64_t id;                        // the list's id, or zero for an entry that holds no list
    PreparedKey key;                    // the state of the walk where the list was last embedded
    uint64_t build;                     // the build that last embedded the list
    uint64_t clears;                    // the atlas's clears when the records were made
    uint64_t pages;                     // the atlas pages that the records read, as a bit set
    PrIRect drawn;                      // the pixels that the records draw into
    bool whole;                         // true if the list lay inside the clip's box, so that nothing was culled or cut
    bool kept;                          // true if the records are kept; a list walked once holds only its key

    // True if the list's records are not kept: they are of kinds that are not kept, or a build embedded the list in two
    // states, as a glyph's list is embedded at each of its places.
    bool never;

    // The kept records, and the texture numbers that the instances name by index plus one.
    RawArray<FragmentInstance> instances;
    RawArray<float> values;
    RawArray<FragmentPaint> paints;
    RawArray<FragmentClip> clips;
    RawArray<uint32_t> tables;
};

// The clip of an instance that reads the clip in force where the list was embedded.
constexpr uint32_t kOuterClip = 0xffffffffu;

// The state that builds share: the textures, and scratch memory.
struct Fragment {
    // What embeds do, and the kept records of embedded lists in a table of a power of two entries, by the hash of each
    // list's id. A list takes the entry of another list whose hash it shares. The table grows when a build embeds more
    // than a quarter of its entries.
    PrDrawFragmentEmbeds embeds;
    RawArray<PreparedList> prepared;
    uint32_t embedded;                  // the lists that the build in progress embedded

    PrDrawFragmentPaths paths;
    uint32_t atlasSize;
    PrDrawGlyphCache* lists;
    PrPathTessellator* tessellator;
    PrPathSparseRasterizer* sparse;
    PrPathPolyline polyline;
    PrDrawAtlas* atlas;
    RawArray<uint32_t> pageTextures;       // the texture of each atlas page
    RawArray<uint32_t> cursors;            // the position of each cell's next segment as a map's grid fills
    uint32_t pagesCreated;                 // number of atlas pages whose textures the host has
    uint64_t build;
    uint32_t nextTexture;
    RawArray<CachedImage> images;
    KeyIndex imageIndex;                // `images`, by imageKey
    RawArray<CachedRamp> ramps;         // this build's ramps; the list's brushes stay put until the build ends
    HashIndex rampIndex;                // `ramps`, by the inputs that decide their colors
    HashIndex tableIndex;               // this build's texture table, by its entries
    KeyIndex layerIndex;                // this build's layers, by their textures
    PrPath* shape;
    PrPath* region;
    PrPathPolyline centerline;
    PrPathPolyline fill;                    // a stroke's outlines (prDrawLowerStroke)
    PrPathPolyline aligned;
    PrPathPolyline strokeRegion;
    PrPathPolyline combined;
    PrPathPolyline cut;
    PrPathPolyline landed;                  // where a projected group's layer lands (prDrawProjectionQuad)
};

// The offset of a Create request's texels in the texel storage. It is used until the build ends and the storage stops
// moving.
struct PendingTexels {
    uint32_t request;
    size_t offset;
};

// The storage that one frame's arrays point into. A backend keeps the storage of destroyed frames for the next frames.
struct FrameStorage {
    RawArray<FragmentInstance> instances;
    RawArray<float> values;
    RawArray<uint8_t> coverage;
    RawArray<float> tileData;
    RawArray<FragmentPaint> paints;
    RawArray<FragmentClip> clips;
    RawArray<uint32_t> clipShapes;
    RawArray<FragmentClipShape> shapes;
    RawArray<FragmentPiece> pieces;
    RawArray<FragmentMap> maps;
    RawArray<float> mapData;
    RawArray<uint32_t> mapIndices;
    RawArray<PrColor16> rampTables;
    RawArray<uint32_t> rampCells;
    RawArray<PrDrawRampPiece> rampPieces;
    RawArray<PrColor16> rampColors;
    RawArray<FragmentTexture> textures;
    RawArray<PrDrawTextureId> hostTextures;
    RawArray<FragmentLayer> layers;
    RawArray<FragmentPass> passes;
    RawArray<FragmentFilter> filters;
    RawArray<float> filterData;
    RawArray<FragmentTextureRequest> requests;
    RawArray<uint8_t> texels;
    RawArray<PendingTexels> pending;
    uint32_t pathFills;                 // number of paths whose coverage the build prepared
    uint32_t beneath;                   // the layer beneath the target, as its index plus one, or zero
};

// The clips in force. They are the intersection of the rectangle clips, in target pixels, and the pixels that it
// touches; the rounded or turned rectangles, which are the walk's clip shapes from `first`; and a mask layer of the
// clips that are neither.
struct Clip {
    PrIRect box;
    PrRect rect;
    uint32_t first;
    uint32_t count;
    uint32_t mask;              // the mask layer's texture, or zero for none
    uint32_t record;            // the clip record's index plus one, once an instance needs it
    bool whole;                 // true if it lets through everything drawn into it, with no record
};

// A group as the walk goes through it. A group with an effect draws into its layer.
struct GroupFrame {
    uint32_t layer;             // the layer's index plus one, or zero if it has none
    PrDrawComposite composite;
    PrDrawBlendMode blendMode;
    const PrDrawGroup* group;
    PrDrawTransform transform;  // the transform at the push; if local, its space to its layer's pixels
    uint32_t under;             // the layer that it draws over, which a filter's backdrop reads, or zero

    // A filter computes in the group's space. A group whose transform rotates or skews that space draws into a layer in
    // its own space, of equal scale on both axes, at the device's resolution.
    bool local;

    PrMatrix3 toDevice;        // local: its layer's pixels to target pixels
    PrAffine2 toLocal;         // local: the group's space to its layer's pixels

    // A projected group (PrDrawGroup::projection) is local, and its layer holds its plane at the projection's scale. It
    // is not a backdrop root: `under` is the layer that it draws over, resampled into its plane, once a backdrop reads
    // it, and `beneath` is that layer, as its index plus one.
    bool projected;
    PrDrawProjection projection;
    uint32_t beneath;

    // A group with a layer of the host's (PrDrawGroup::layer) is local, and its layer is the host's texture in its own
    // texels. `drawn` is the revision that the texture held when the walk claimed the group's contents
    // (drawSharedClaimLayer).
    bool host;
    uint64_t drawn;

    bool hidden;                // its contents draw nothing that shows (pushHiddenGroup)
};

// A texture that reads as one layer over another, or over a color. It serves as a filter's input if the list's layer
// lies over what lies beneath the target. It has a texture number of its own, which the table resolves.
struct Stacked {
    uint32_t texture;
    uint32_t top;               // the layer's texture
    uint32_t under;             // the texture beneath it, or zero for the color
    PrColor16 color;            // premultiplied
};

// The state of one build as it walks the commands.
struct Walk {
    Fragment* f;
    FrameStorage* b;
    PrIRect target;
    bool extended;              // true if the target keeps colors outside [0, 1]
    RawArray<PrDrawTransform> transforms;
    RawArray<Clip> clips;          // innermost last
    RawArray<uint32_t> clipShapes; // the clips' shapes, each an index into the frame's shapes
    RawArray<float> opacities;     // the product of the opacities of groups without layers, innermost last
    RawArray<uint32_t> layers;     // the layers drawn into, each its index plus one, innermost last; zero is the target
    RawArray<bool> knockouts;      // true for each of `layers` that belongs to a knockout group

    // For each of `layers`, true if the walk keeps the bounds of what has been drawn into it so far in `drawn`. Only
    // the list's own layer and projected groups' layers need them.
    RawArray<bool> tracks;
    RawArray<PrIRect> drawn;
    RawArray<GroupFrame> groups;
    uint32_t pass;              // the pass being built
    PrDrawComposite composite;  // the operator that the records being made composite by
    PrDrawBlendMode blendMode;
    const PrDrawBeneath* PR_NULLABLE beneath; // what lies beneath the target, or null for transparency

    // The layer that the list draws into, as its index plus one, or zero for the target. That layer is entered first
    // and stays entered while the list draws, so its entry in `drawn` is the second.
    uint32_t root;
    RawArray<Stacked> stacked;
    RawArray<PrColor> rampColors;  // a ramp's colors before they are recorded
};

const Clip& clipOf(const Walk& w) {
    return w.clips.last();
}

float opacityOf(const Walk& w) {
    return w.opacities.last();
}

// Returns the target pixels that a box of a layer's pixels covers through the projective transform, within `limit`. The
// result includes one more pixel for sampling between pixels. Returns the whole limit if the transform takes a corner
// past infinity.
PrIRect mappedBox(const PrMatrix3& matrix, PrIRect box, PrIRect limit) {
    float left = kPrF32Infinity, top = kPrF32Infinity, right = -kPrF32Infinity, bottom = -kPrF32Infinity;
    const float xs[2] = {float(box.left), float(box.right)};
    const float ys[2] = {float(box.top), float(box.bottom)};
    for (float x : xs) {
        for (float y : ys) {
            PrVec2 p{};
            if (!prMatrix3MapPoint(&matrix, PrVec2{x, y}, &p))
                return limit;
            left = prF32Min(left, p.x);
            top = prF32Min(top, p.y);
            right = prF32Max(right, p.x);
            bottom = prF32Max(bottom, p.y);
        }
    }
    return prIRectIntersect(prIRectOutset(prIRectRoundOut(PrRect{left, top, right, bottom}), 1), limit);
}

// Returns the premultiplied color as the frame's records hold it. A frame for an 8-bit target holds it in [0, 1], and
// any frame holds alpha in [0, 1].
PrColor16 recordColor(const Walk& w, PrColor c) {
    float a = prF32Clamp(c.a, 0, 1);
    if (!w.extended)
        return PrColor{prF32Clamp(c.r, 0, a), prF32Clamp(c.g, 0, a), prF32Clamp(c.b, 0, a), a};
    return PrColor{c.r, c.g, c.b, a};
}

uint64_t colorBits(PrColor16 c) {
    return uint64_t(c.r) | uint64_t(c.g) << 16 | uint64_t(c.b) << 32 | uint64_t(c.a) << 48;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures and requests

void request(Walk& w, const FragmentTextureRequest& r) {
    w.b->requests.push(r);
}

// Returns the texture of the image, premultiplied, with every mip level if `mipmapped`, which the device fills. An
// image in half floats keeps them, and any other is Rgba8. The texture is created the first time a build draws the
// image. Returns 0 for an image with no pixels.
uint32_t textureOfImage(Walk& w, const PrDrawImage* image, bool mipmapped = false) {
    Fragment* f = w.f;
    uint64_t id = prDrawImageId(image);
    uint64_t key = imageKey(id, mipmapped);
    uint32_t found = f->imageIndex.find(key, [&](uint32_t i) {
        return imageKey(f->images.data[i].id, f->images.data[i].mipmapped);
    });
    if (found != kHashMissing) {
        CachedImage& cached = f->images.data[found];
        cached.lastUsed = f->build;
        return cached.texture;
    }
    uint32_t width = prDrawImageExtent(image).x;
    uint32_t height = prDrawImageExtent(image).y;
    if (width == 0 || height == 0)
        return 0;
    bool half = prDrawImageFormat(image) == PrPixelFormat_RgbaF16;
    PrPixelFormat format = half ? PrPixelFormat_RgbaF16 : PrPixelFormat_Rgba8;
    size_t stride = size_t(width) * prPixelFormatSize(format);
    size_t size = stride * height;
    size_t offset = w.b->texels.count;
    // The frame's texels are addressed in 32 bits. An image beyond that limit draws nothing.
    if (size > UINT32_MAX - offset)
        return 0;
    w.b->texels.reserve(uint32_t(offset + size));
    w.b->texels.count = uint32_t(offset + size);
    uint8_t* texels = w.b->texels.data + offset;
    prMemZero(texels, size);
    PrPixels pixels{texels, stride, {width, height}};
    prDrawImageWritePixels(image, pixels, format, PrIVec2{0, 0});
    uint32_t texture = ++f->nextTexture;
    uint32_t index = w.b->requests.count;
    request(w, FragmentTextureRequest{
        .op = FragmentTextureOp_Create,
        .format = format,
        .mipmapped = mipmapped,
        .texture = texture,
        .texels = PrPixelView{nullptr, stride, {width, height}},
    });
    w.b->pending.push(PendingTexels{index, offset});
    f->imageIndex.insert(key, f->images.count);
    f->images.push(CachedImage{id, mipmapped, texture, f->build});
    return texture;
}

// Returns the index of the gradient's ramp for the color. The ramp is written into the frame the first time this build
// asks for it, for this gradient or any gradient with the same ramp.
uint32_t rampOf(Walk& w, const PrDrawBrush* brush, PrColor color) {
    Fragment* f = w.f;
    const uint32_t hash = uint32_t(prDrawGradientRampHash(brush, color));
    const uint32_t found = f->rampIndex.find(hash, [&](uint32_t i) {
        return prDrawGradientRampEquals(f->ramps.data[i].brush, f->ramps.data[i].color, brush, color);
    });
    if (found != kHashMissing)
        return f->ramps.data[found].index;
    f->rampIndex.insert(hash, f->ramps.count);
    RawArray<PrDrawRampPiece>& pieces = w.b->rampPieces;
    const uint32_t first = pieces.count;
    pieces.resize(first + prDrawGradientRampMaxPieces(brush->stopCount));
    RawArray<PrColor>& colors = w.rampColors;
    colors.resize(prDrawGradientRampMaxColors(brush->stopCount, kFragmentRampResolution));
    uint32_t count =
        prDrawGradientRamp(brush, color, PrColorSpace_Srgb, kFragmentRampResolution, pieces.data + first, colors.data);
    pieces.resize(first + count);
    colors.resize(prDrawRampColorCount(pieces.data + first, count));
    // The table holds the colors at its steps. A step that two pieces meet within, or at the end of, is marked with the
    // piece that holds its start, so a shader finds the exact color there from the pieces.
    constexpr uint32_t kSteps = kFragmentRampResolution;
    // The steps rise through the pieces in order, as prDrawRampPieceAt and prDrawRampColor find them.
    const PrDrawRampPiece* ramp = pieces.data + first;
    uint32_t piece = 0;
    for (uint32_t i = 0; i <= kSteps; ++i) {
        float t = float(i) / float(kSteps);
        while (t > ramp[piece].end || (t == ramp[piece].end && t < 1))
            ++piece;
        const PrDrawRampPiece& p = ramp[piece];
        const PrColor* c = colors.data + p.first;
        PrColor at = c[0];
        if (p.count > 1) {
            float x = (t - p.start) / (p.end - p.start) * float(p.count - 1);
            float k = prF32Clamp(prF32Floor(x), 0, float(p.count - 2));
            float u = prF32Clamp(x - k, 0, 1);
            const PrColor& a = c[uint32_t(k)];
            const PrColor& b = c[uint32_t(k) + 1];
            at = PrColor{a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u, a.a + (b.a - a.a) * u};
        }
        w.b->rampTables.push(recordColor(w, at));
        if (i == kSteps)
            break;
        float end = float(i + 1) / float(kSteps);
        bool meets = p.end < end || (p.end == end && end < 1);
        w.b->rampCells.push(meets ? first + piece + 1 : 0);
    }
    w.b->rampCells.push(first + 1);
    w.b->rampCells.push(first + count);
    // The pieces name colors in the frame's colors.
    const uint32_t base = w.b->rampColors.count;
    for (uint32_t i = first; i < pieces.count; ++i)
        pieces.data[i].first += base;
    for (uint32_t i = 0; i < colors.count; ++i)
        w.b->rampColors.push(recordColor(w, colors.data[i]));
    uint32_t index = w.b->rampTables.count / (kSteps + 1) - 1;
    f->ramps.push(CachedRamp{brush, color, index});
    return index;
}

// ---------------------------------------------------------------------------------------------------------------------
// Instances and records

// Returns the box of the surface being drawn into: the current layer's box, or the target's.
PrIRect targetBox(const Walk& w) {
    uint32_t layer = w.layers.last();
    if (layer == 0)
        return w.target;
    const FragmentLayer& l = w.b->layers.data[layer - 1];
    return PrIRect{l.origin.x, l.origin.y, l.origin.x + int32_t(l.extent.x), l.origin.y + int32_t(l.extent.y)};
}

// Returns the texture as the frame's instances and records name it: its index in the frame's textures plus one, or zero
// for none. The entry reads the texture over the entry that `under` names, or over the color if `under` is zero. The
// entry is added on first use. A layer's entry holds the layer's position.
uint32_t tableEntry(Walk& w, uint32_t texture, uint32_t under, PrColor16 color) {
    RawArray<FragmentTexture>& table = w.b->textures;
    FragmentTexture entry{};
    entry.texture = texture;
    entry.under = under;
    entry.color = color;
    uint32_t hash = uint32_t(prHashFold(uint64_t(texture) | uint64_t(under) << 32, colorBits(color)));
    uint32_t found = w.f->tableIndex.find(hash, [&](uint32_t i) {
        const FragmentTexture& e = table.data[i];
        return e.hostTexture == 0 && e.texture == texture && e.under == under && colorBits(e.color) == colorBits(color);
    });
    if (found != kHashMissing)
        return found + 1;
    uint32_t layer = w.f->layerIndex.find(texture, [&](uint32_t i) { return uint64_t(w.b->layers.data[i].texture); });
    if (layer != kHashMissing) {
        const FragmentLayer& l = w.b->layers.data[layer];
        entry.origin.x = l.origin.x;
        entry.origin.y = l.origin.y;
        entry.extent.x = l.extent.x;
        entry.extent.y = l.extent.y;
    }
    w.f->tableIndex.insert(hash, table.count);
    table.push(entry);
    return table.count;
}

uint32_t tableOf(Walk& w, uint32_t texture) {
    if (texture == 0)
        return 0;
    for (uint32_t i = 0; i < w.stacked.count; ++i) {
        const Stacked& t = w.stacked.data[i];
        if (t.texture == texture)
            return tableEntry(w, t.top, tableOf(w, t.under), t.color);
    }
    return tableEntry(w, texture, 0, PrColor16{});
}

// The paint of a command's instances: their premultiplied color, and their paint record.
struct Ink {
    PrColor16 color;
    uint32_t paint;             // the paint record's index plus one, or zero for the color alone
    bool reads;                 // true if its compositing reads the destination
};

// Returns a paint record of the color alone, composited with the walk's current operator and blend mode.
FragmentPaint plainPaint(const Walk& w) {
    FragmentPaint r{};
    r.brush = prMatrix3FromAffine2(prAffine2Identity());
    r.kind = FragmentPaintKind_Solid;
    r.composite = w.composite;
    r.blendMode = w.blendMode;
    r.knockout = w.knockouts.last() ? 1 : 0;
    return r;
}

// Returns the ink of the record and the premultiplied color. The record is added only if the color alone cannot paint
// it.
Ink inkOf(Walk& w, const FragmentPaint& r, const float color[4]) {
    Ink ink{recordColor(w, PrColor{color[0], color[1], color[2], color[3]}), 0, false};
    ink.reads = r.composite != PrDrawComposite_SourceOver || r.blendMode != PrDrawBlendMode_Normal || r.knockout != 0;
    if (r.kind != FragmentPaintKind_Solid || ink.reads || r.groupMask != FragmentMask_None || r.map != 0) {
        w.b->paints.push(r);
        ink.paint = w.b->paints.count;
    }
    return ink;
}

// Returns the ink with its color scaled by `factor`.
Ink fadedInk(Ink ink, float factor) {
    float f = prF32Clamp(factor, 0, 1);
    PrColor c = ink.color;
    ink.color = PrColor{c.r * f, c.g * f, c.b * f, c.a * f};
    return ink;
}

// Computes the paint of a brush times a color, for a command whose fill geometry has `bounds` in the current space, and
// the premultiplied color that its instances carry. Returns false if it paints nothing.
bool makePaint(Walk& w, const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds,
    FragmentPaint* out, float premultiplied[4], const PrVec2* PR_NULLABLE unitPixels = nullptr);

uint32_t enterLayer(Walk& w, PrIRect box, bool clipped, bool precise = false);
PrIRect leaveLayer(Walk& w);
void runCommands(Walk& w, const PrDrawCommand* commands, uint32_t count);

// The maximum number of texels along a side of a list rendered as a source.
constexpr uint32_t kListTextureMaxSize = 8192;

// A source as a texture: the texture's size, and the source's rectangle of its texels.
struct SourceTexture {
    uint32_t texture;
    PrDrawTextureId hostTexture;               // the host's texture, instead of `texture`, or zero
    float width;
    float height;
    PrRect texels;
    PrDrawAlpha alpha;
};

FragmentLayer& layerRecord(Walk& w, uint32_t texture);
void mipmapLayer(Walk& w, uint32_t texture);

// Gets the source as a texture: an image's texels, the host's texture, or a list's area rendered into a layer the size
// of `tile` at `scale` pixels per unit along each axis. An image or a list sampled by Mipmap gets every mip level. A
// host texture has the levels that the host gave it. Returns false if the source has no texture.
bool sourceTexture(Walk& w, const PrDrawSource& source, PrRect tile, PrVec2 scale, PrDrawSampling sampling,
        SourceTexture* out) {
    bool mipmapped = (sampling & PrDrawSamplingFlag_Kind) == PrDrawSampling_Mipmap;
    if (source.kind == PrDrawSourceKind_Texture) {
        *out = SourceTexture{
            0, source.texture, float(source.extent.x), float(source.extent.y), source.rect,
            source.alpha,
        };
        return true;
    }
    if (source.kind == PrDrawSourceKind_Image) {
        uint32_t texture = textureOfImage(w, source.image, mipmapped);
        *out = SourceTexture{
            texture, 0, float(prDrawImageExtent(source.image).x),
            float(prDrawImageExtent(source.image).y), source.rect, PrDrawAlpha_Premultiplied,
        };
        return texture != 0;
    }
    if (source.kind != PrDrawSourceKind_List || source.list == nullptr || !(scale.x > 0) || !(scale.y > 0) ||
            !prF32IsFinite(scale.x) || !prF32IsFinite(scale.y))
        return false;
    const PrRect& area = source.rect;
    PrUVec2 layer = prDrawLayerExtent(PrVec2{prRectWidth(tile), prRectHeight(tile)}, scale, kListTextureMaxSize);
    if (layer.x == 0 || !(area.right > area.left && area.bottom > area.top))
        return false;
    float width = float(layer.x);
    float height = float(layer.y);
    // The area fills the layer, which draws with source-over and normal blending, whatever the command's operator.
    PrDrawComposite composite = w.composite;
    PrDrawBlendMode blendMode = w.blendMode;
    w.composite = PrDrawComposite_SourceOver;
    w.blendMode = PrDrawBlendMode_Normal;
    uint32_t texture = enterLayer(w, PrIRect{0, 0, int32_t(width), int32_t(height)}, false);
    w.transforms.push(prDrawTransformFromAffine2(prAffine2Concat(prAffine2Translate(PrVec2{-area.left, -area.top}),
        prAffine2Scale(PrVec2{width / prRectWidth(area), height / prRectHeight(area)}))));
    runCommands(w, prDrawListCommands(source.list), prDrawListCommandCount(source.list));
    w.transforms.pop();
    leaveLayer(w);
    w.composite = composite;
    w.blendMode = blendMode;
    // The layer is sampled as a whole texture, so its tiles wrap at its edges.
    if (mipmapped)
        mipmapLayer(w, texture);
    else
        layerRecord(w, texture).exact = true;
    *out = SourceTexture{texture, 0, width, height, PrRect{0, 0, width, height}, PrDrawAlpha_Premultiplied};
    return true;
}

// Returns how an Image paint reads its texture. The hardware's samplers serve every case except two: a filter whose
// taps must stay within a part of the texture, unless the sampling has a gutter (PrDrawSamplingFlag_Gutter), and an
// opaque texture whose taps can land on no texel, since the samplers' transparent texels would read as opaque. A copy
// that is filtered on its own (PrDrawTileFlag_Clamped) is read as a tile that extends by Pad. The shader leaves the
// gaps between such copies empty.
FragmentImageRead imageRead(PrDrawSampling sampling, PrDrawAlpha alpha, const PrDrawTiles& tiles,
        PrRect texels, float width, float height) {
    PrDrawSampling kind = PrDrawSampling(sampling & PrDrawSamplingFlag_Kind);
    if (kind == PrDrawSampling_Nearest)
        return FragmentImageRead_Sampler;
    bool gutter = (sampling & PrDrawSamplingFlag_Gutter) != 0;
    bool clamped = (tiles.flags & PrDrawTileFlag_Clamped) != 0;
    auto copied = [&](PrDrawExtend mode) -> PrDrawExtend {
        return clamped && (mode == PrDrawExtend_Repeat || mode == PrDrawExtend_Reflect) ? PrDrawExtend(PrDrawExtend_Pad) : mode;
    };
    const PrDrawExtend modes[2] = {copied(tiles.x), copied(tiles.y)};
    const bool spaced[2] = {!clamped && tiles.spacing.x > 0, !clamped && tiles.spacing.y > 0};
    const bool whole[2] = {texels.left == 0 && texels.right == width, texels.top == 0 && texels.bottom == height};
    for (uint32_t axis = 0; axis < 2; ++axis) {
        bool wraps = modes[axis] == PrDrawExtend_Repeat || modes[axis] == PrDrawExtend_Reflect;
        bool transparent = modes[axis] == PrDrawExtend_None || (wraps && spaced[axis]);
        if (alpha == PrDrawAlpha_Opaque && transparent)
            return FragmentImageRead_Texels;
        // Pad within a part of the texture stays exact only for a bilinear filter of the first level, whose taps beyond
        // the clamped point have no weight.
        if (!whole[axis] && !gutter && (modes[axis] != PrDrawExtend_Pad || kind != PrDrawSampling_Linear))
            return FragmentImageRead_Texels;
    }
    return FragmentImageRead_Sampler;
}

// Returns the source's texture as the frame's instances and records name it. A host texture takes an entry of its own.
uint32_t tableOfSource(Walk& w, const SourceTexture& source) {
    if (source.hostTexture == 0)
        return tableOf(w, source.texture);
    w.b->hostTextures.push(source.hostTexture);
    FragmentTexture entry{};
    entry.hostTexture = w.b->hostTextures.count;
    w.b->textures.push(entry);
    return w.b->textures.count;
}

bool makePaint(Walk& w, const PrDrawBrush* PR_NULLABLE brush, PrColor color, PrRect bounds,
        FragmentPaint* out, float premultiplied[4], const PrVec2* PR_NULLABLE unitPixels) {
    FragmentPaint& r = *out;
    r = plainPaint(w);
    float opacity = opacityOf(w);
    float a = prF32Clamp(color.a, 0, 1) * opacity;
    premultiplied[0] = (w.extended ? color.r : prF32Clamp(color.r, 0, 1)) * a;
    premultiplied[1] = (w.extended ? color.g : prF32Clamp(color.g, 0, 1)) * a;
    premultiplied[2] = (w.extended ? color.b : prF32Clamp(color.b, 0, 1)) * a;
    premultiplied[3] = a;
    if (brush == nullptr) {
        // Into 8-bit pixels, a color is rounded to what they hold before coverage applies, so that colors that round to
        // one color paint alike, at antialiased edges too.
        if (!w.extended) {
            for (uint32_t k = 0; k < 4; ++k)
                premultiplied[k] = prF32Floor(prF32Clamp(premultiplied[k], 0, 1) * 255 + 0.5f) / 255;
        }
        return a > 0;
    }
    // Maps target pixels to brush space.
    const PrDrawTransform& x = w.transforms.last();
    PrAffine2 fromSpace{};
    if (!prDrawBrushFromTarget(brush, x.affine, bounds, &fromSpace))
        return false;
    r.brush = prMatrix3FromAffine2(fromSpace);
    r.extend = brush->extend;
    switch (brush->kind) {
        case PrDrawBrushKind_Image: {
            const PrDrawSource& source = brush->image.source;
            const PrDrawTiles& tiles = brush->image.tiles;
            if (!(tiles.tile.right > tiles.tile.left && tiles.tile.bottom > tiles.tile.top))
                return false;
            // A list renders at the number of target pixels that a unit of brush space covers. Along a mapped stroke,
            // it renders at the stroke's size.
            PrVec2 scale{};
            if (unitPixels != nullptr) {
                const PrAffine2& t = brush->transform;
                float ax = t.a * unitPixels->x;
                float by = t.b * unitPixels->y;
                float cx = t.c * unitPixels->x;
                float dy = t.d * unitPixels->y;
                scale = PrVec2{prF32Sqrt(ax * ax + by * by), prF32Sqrt(cx * cx + dy * dy)};
            } else if (source.kind == PrDrawSourceKind_List && source.list != nullptr) {
                PrAffine2 toDevice{};
                if (!prAffine2Invert(fromSpace, &toDevice))
                    return false;
                float uniform = prAffine2MaxScale(toDevice);
                scale = PrVec2{uniform, uniform};
            }
            SourceTexture texture{};
            if (!sourceTexture(w, source, tiles.tile, scale, brush->image.sampling, &texture))
                return false;
            r.kind = FragmentPaintKind_Image;
            r.sampling = brush->image.sampling & PrDrawSamplingFlag_Kind;
            r.read =
                imageRead(brush->image.sampling, texture.alpha, tiles, texture.texels, texture.width, texture.height);
            r.alpha = texture.alpha;
            r.limit = brush->image.limit;
            r.tileX = tiles.x;
            r.tileY = tiles.y;
            r.tileFlags = tiles.flags;
            r.tile = tiles.tile;
            r.spacing = PrVec2{prF32Max(tiles.spacing.x, 0), prF32Max(tiles.spacing.y, 0)};
            r.texels = texture.texels;
            r.texture = tableOfSource(w, texture);
            return a > 0;
        }
        case PrDrawBrushKind_Linear: {
            r.kind = FragmentPaintKind_Linear;
            float g[6] = {brush->linear.start.x, brush->linear.start.y, brush->linear.end.x, brush->linear.end.y, 0, 0};
            prMemCopy(r.gradient, g, sizeof(g));
            break;
        }
        case PrDrawBrushKind_Radial: {
            r.kind = FragmentPaintKind_Radial;
            float g[6] = {
                brush->radial.end.x, brush->radial.end.y, brush->radial.endRadius,
                brush->radial.start.x,  brush->radial.start.y,  brush->radial.startRadius,
            };
            prMemCopy(r.gradient, g, sizeof(g));
            break;
        }
        case PrDrawBrushKind_Conic: {
            r.kind = FragmentPaintKind_Conic;
            float g[6] = {brush->conic.center.x, brush->conic.center.y, brush->conic.angle, 0, 0, 0};
            prMemCopy(r.gradient, g, sizeof(g));
            break;
        }
        default: {
            r.kind = FragmentPaintKind_Diamond;
            float g[6] = {
                brush->diamond.center.x, brush->diamond.center.y, brush->diamond.radii.x,
                brush->diamond.radii.y,  0,                        0,
            };
            prMemCopy(r.gradient, g, sizeof(g));
            break;
        }
    }
    // The ramp carries the color. The premultiplied carries the groups' opacity.
    r.ramp = rampOf(w, brush, color);
    for (uint32_t i = 0; i < 4; ++i)
        premultiplied[i] = opacity;
    return opacity > 0;
}

// Returns the target pixels that a rectangle of the current space reaches.
PrIRect deviceBox(const Walk& w, PrRect r) {
    return prIRectRoundOut(prAffine2MapRect(w.transforms.last().affine, r));
}

// Returns the record of the clips in force, which is created the first time an instance needs it. Returns zero if the
// clips let through everything that is drawn.
uint32_t clipRecord(Walk& w) {
    Clip& c = w.clips.last();
    if (c.whole)
        return 0;
    if (c.record == 0) {
        FragmentClip r{};
        r.rect = c.rect;
        r.shapes = w.b->clipShapes.count;
        r.shapeCount = c.count;
        w.b->clipShapes.append(w.clipShapes.data + c.first, c.count);
        r.mask = tableOf(w, c.mask);
        w.b->clips.push(r);
        c.record = w.b->clips.count;
    }
    return c.record;
}

// Adds the instance, painting with the ink under the clips in force, if any pixel of `box` shows through them. Returns
// false if no pixel shows through.
bool emit(Walk& w, FragmentInstance instance, const Ink& ink, const PrIRect& box) {
    if (prIRectIsEmpty(prIRectIntersect(box, clipOf(w).box)))
        return false;
    instance.color = ink.color;
    instance.paint = ink.paint;
    if (ink.reads)
        instance.flags |= FragmentInstanceFlag_ReadsDestination;
    instance.clip = clipRecord(w);
    uint32_t kind = instance.flags & FragmentInstanceFlag_Coverage;
    bool covers = kind == FragmentCoverage_Solid || kind == FragmentCoverage_Rect ||
        kind == FragmentCoverage_Triangle || kind == FragmentCoverage_Atlas;
    const FragmentClip* clip = instance.clip != 0 ? &w.b->clips.data[instance.clip - 1] : nullptr;
    if (covers && ink.paint == 0 && !ink.reads && (clip == nullptr || (clip->shapeCount == 0 && clip->mask == 0)))
        instance.flags |= FragmentInstanceFlag_Basic;
    w.b->instances.push(instance);
    if (w.tracks.last())
        w.drawn.last() = prIRectUnion(w.drawn.last(), prIRectIntersect(box, clipOf(w).box));
    return true;
}

// Returns an instance of the kind over a box of target pixels. Its local space is the box's pixels from its top left.
FragmentInstance boxInstance(PrIRect b, FragmentCoverage kind) {
    FragmentInstance i{};
    i.transform = prAffine2Translate(PrVec2{float(b.left), float(b.top)});
    i.bounds = PrRect{0, 0, float(prIRectWidth(b)), float(prIRectHeight(b))};
    i.flags = kind;
    return i;
}

// Covers the pixels of the box within the clip's box with the ink.
void drawSolid(Walk& w, PrIRect box, const Ink& ink) {
    box = prIRectIntersect(box, clipOf(w).box);
    if (!prIRectIsEmpty(box))
        emit(w, boxInstance(box, FragmentCoverage_Solid), ink, box);
}

// Adds an instance of a shape kind over `bounds` in local space, which `toDevice` maps to target pixels. The instance
// reads the record from the values.
template <typename Record>
void drawShaped(Walk& w, FragmentCoverage kind, PrAffine2 toDevice, PrRect bounds, const Record& record, const Ink& ink) {
    static_assert(sizeof(Record) % sizeof(float) == 0);
    FragmentInstance i{};
    i.transform = toDevice;
    i.bounds = bounds;
    i.flags = kind;
    i.data = w.b->values.count;
    if (emit(w, i, ink, prIRectRoundOut(prAffine2MapRect(toDevice, bounds)))) {
        RawArray<float>& values = w.b->values;
        uint32_t at = values.count;
        values.reserve(at + uint32_t(sizeof(Record) / sizeof(float)));
        values.count = at + uint32_t(sizeof(Record) / sizeof(float));
        prMemCopy(values.data + at, &record, sizeof(Record));
    }
}

// Returns the rectangle grown by `pixels` target pixels along each axis of its space, using the scale of `toDevice`.
PrRect grownByPixels(PrRect r, PrAffine2 toDevice, float pixels) {
    float sx = prF32Sqrt(toDevice.a * toDevice.a + toDevice.b * toDevice.b);
    float sy = prF32Sqrt(toDevice.c * toDevice.c + toDevice.d * toDevice.d);
    float mx = sx > 0 ? pixels / sx : 0;
    float my = sy > 0 ? pixels / sy : 0;
    return PrRect{r.left - mx, r.top - my, r.right + mx, r.bottom + my};
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

// Draws the mesh in target pixels with the ink, with one instance for each triangle.
void drawMesh(Walk& w, const PrPathMesh& mesh, const Ink& ink) {
    ++w.b->pathFills;
    if (mesh.indexCount == 0 || prIRectIsEmpty(clipOf(w).box))
        return;
    PrIRect everywhere = clipOf(w).box;
    for (uint32_t k = 0; k + 2 < mesh.indexCount; k += 3) {
        FragmentInstance i{};
        for (uint32_t c = 0; c < 3; ++c) {
            uint32_t v = mesh.indices[k + c];
            i.corners[c] = mesh.points[v];
            i.coverages[c] = mesh.coverage[v];
        }
        i.flags = FragmentCoverage_Triangle;
        emit(w, i, ink, everywhere);
    }
}

// Fills a polyline in target pixels with quads of sparse coverage within the clip's box.
void fillSparse(Walk& w, const PrPathPolyline& polyline, PrPathFillRule rule, const Ink& ink) {
    if (polyline.pointCount == 0)
        return;
    PrIRect covered = prIRectRoundOut(prPathPolylineBounds(&polyline));
    PrIRect box = prIRectIntersect(clipOf(w).box, covered);
    if (prIRectIsEmpty(box))
        return;
    ++w.b->pathFills;
    PrPathSparse sparse = prPathSparseRasterizerFill(w.f->sparse, &polyline, rule, box);
    if (sparse.spanCount == 0)
        return;
    uint32_t coverage = w.b->coverage.count;
    uint32_t tiles = w.b->tileData.count;
    if (sparse.coverageCount != 0)
        w.b->coverage.append(sparse.coverage, sparse.coverageCount);
    if (sparse.tileDataCount != 0)
        w.b->tileData.append(sparse.tileData, sparse.tileDataCount);
    FragmentCoverage tileKind =
        rule == PrPathFillRule_EvenOdd ? FragmentCoverage_TilesEvenOdd : FragmentCoverage_TilesNonZero;
    for (uint32_t i = 0; i < sparse.spanCount; ++i) {
        const PrPathSparseSpan& s = sparse.spans[i];
        PrIRect span{s.origin.x, s.origin.y, s.origin.x + int32_t(s.extent.x), s.origin.y + int32_t(s.extent.y)};
        if (s.data == kPrPathSparseSolid) {
            emit(w, boxInstance(span, FragmentCoverage_Solid), ink, span);
        } else if (w.f->paths == PrDrawFragmentPaths_Strips) {
            FragmentInstance q = boxInstance(span, FragmentCoverage_Bytes);
            q.data = coverage + s.data;
            emit(w, q, ink, span);
        } else {
            // One instance for each tile. Each tile's record follows the record of the tile before it.
            uint32_t record = s.data;
            for (uint32_t x = 0; x < s.extent.x; x += kSegmentTile) {
                uint32_t width = s.extent.x - x < kSegmentTile ? s.extent.x - x : kSegmentTile;
                PrIRect tile{span.left + int32_t(x), span.top, span.left + int32_t(x + width), span.bottom};
                FragmentInstance q = boxInstance(tile, tileKind);
                q.data = tiles + record;
                emit(w, q, ink, tile);
                record += 1 + s.extent.y + uint32_t(sparse.tileData[record]) * 4;
            }
        }
    }
}

// Returns true if a fill is sparse instead of tessellated.
bool sparseFill(const Walk& w, bool antialias) {
    return antialias && w.f->paths != PrDrawFragmentPaths_Tessellate;
}

// Returns the tolerance of an outline, depending on whether `sparseFill` selects a sparse fill.
float toleranceOf(const Walk& w, bool antialias) {
    return sparseFill(w, antialias) ? kSparseTolerance : kTolerance;
}

// Flattens the path into `out` in target pixels, under the current transform, within `tolerance`, and returns `out`. A
// fill's curves keep their area.
const PrPathPolyline& land(Walk& w, const PrPath* path, float tolerance, PrPathFlattening flattening, PrPathPolyline& out) {
    const PrDrawTransform& x = w.transforms.last();
    prPathPolylineClear(&out);
    prPathFlatten(path, x.affine, tolerance, flattening, &out);
    return out;
}

// Fills a polyline in target pixels.
void fillDevice(Walk& w, const PrPathPolyline& polyline, PrPathFillRule rule, bool antialias, const Ink& ink) {
    if (sparseFill(w, antialias)) {
        fillSparse(w, polyline, rule, ink);
        return;
    }
    PrPathMesh mesh = prPathTessellatorFill(w.f->tessellator, &polyline, rule, antialias ? kFringe : 0);
    drawMesh(w, mesh, ink);
}

// Fills a path in the current space.
void fillPath(Walk& w, const PrPath* path, PrPathFillRule rule, bool antialias, const Ink& ink) {
    const PrPathPolyline& landed = land(w, path, toleranceOf(w, antialias), PrPathFlattening_Balanced, w.f->polyline);
    fillDevice(w, landed, rule, antialias, ink);
}

// Computes the stroke drawn for `stroke` in the current space, and its ink. A stroke narrower than a pixel is drawn a
// pixel wide, and its ink fades with its width. Returns false if it draws nothing.
bool drawnStroke(const Walk& w, const PrDrawStroke* stroke, bool antialias, PrDrawStroke* out, Ink* ink) {
    if (!(stroke->shape.width > 0))
        return false;
    float scale = prAffine2MaxScale(w.transforms.last().affine);
    if (!(scale > 0))
        return false;
    *out = *stroke;
    float device = out->nonScaling ? out->shape.width : out->shape.width * scale;
    if (antialias && device < 1) {
        *ink = fadedInk(*ink, device);
        out->shape.width = out->nonScaling ? 1 : out->shape.width / device;
    }
    return true;
}

// Strokes a path in the current space, with the stroke that drawnStroke computes.
void strokePath(Walk& w, const PrPath* shape, const PrDrawStroke* stroke, bool antialias, Ink ink) {
    Fragment* f = w.f;
    const PrDrawTransform& x = w.transforms.last();
    PrDrawStroke s{};
    if (!drawnStroke(w, stroke, antialias, &s, &ink))
        return;
    float tolerance = toleranceOf(w, antialias);
    prPathPolylineClear(&f->fill);
    prPathPolylineClear(&f->aligned);
    prPathPolylineClear(&f->strokeRegion);
    prDrawLowerStroke(shape, &s, x.affine, tolerance, &f->fill, &f->aligned, &f->strokeRegion);
    if (f->aligned.pointCount != 0) {
        // Cuts the aligned outline to the region's interior or exterior.
        prPathPolylineClear(&f->combined);
        PrPathPolylineOp op =
            s.align == PrDrawStrokeAlign_Inside ? PrPathPolylineOp_Intersect : PrPathPolylineOp_Difference;
        prPathPolylineCombine(&f->aligned, PrPathFillRule_NonZero, &f->strokeRegion, PrPathFillRule_NonZero, op,
            &f->combined);
        fillDevice(w, f->combined, PrPathFillRule_NonZero, antialias, ink);
    }
    if (f->fill.pointCount != 0)
        fillDevice(w, f->fill, PrPathFillRule_NonZero, antialias, ink);
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes that instances and clips evaluate

// Returns true if the corner is round or square. A shape's radii can describe only those corners.
bool radiusCorner(const PrCorner& corner) {
    return prCornerIsSquare(corner) || corner.shape == 1;
}

// A rectangle with shaped corners in an instance's space, in the form that FragmentCornerRect describes.
struct InstanceCornerRect {
    PrRect rect;
    float radii[8];
    float corners[4] = {1, 1, 1, 1};
};

FragmentCornerRect recordOf(const InstanceCornerRect& shape) {
    FragmentCornerRect out{};
    out.rect = shape.rect;
    for (uint32_t i = 0; i < PrRectCorner_Count; ++i) {
        out.radii[i] = PrVec2{shape.radii[i * 2], shape.radii[i * 2 + 1]};
        out.shapes[i] = shape.corners[i];
    }
    return out;
}

// Returns the shape with its corners fitted.
InstanceCornerRect fittedCorners(const PrCornerRect& shape) {
    PrCornerRect fittedShape = prCornerRectFit(shape);
    InstanceCornerRect out{shape.rect, {}};
    for (uint32_t i = 0; i < 4; ++i) {
        const PrCorner& corner = fittedShape.corners[i];
        bool square = prCornerIsSquare(corner);
        out.radii[i * 2] = square ? 0 : corner.radii.x;
        out.radii[i * 2 + 1] = square ? 0 : corner.radii.y;
        out.corners[i] = square ? 1 : corner.shape;
    }
    return out;
}

// Writes the fitted radii of the rectangle's corners, horizontal and vertical, clockwise from the top left. Returns
// false if a corner is neither round nor square.
bool fittedRadii(const PrCornerRect& shape, float radii[8]) {
    PrCornerRect fittedShape = prCornerRectFit(shape);
    const PrCorner* fitted = fittedShape.corners;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!radiusCorner(fitted[i]))
            return false;
        bool square = prCornerIsSquare(fitted[i]);
        radii[i * 2] = square ? 0 : fitted[i].radii.x;
        radii[i * 2 + 1] = square ? 0 : fitted[i].radii.y;
    }
    return true;
}

// Writes the rectangle with fitted corners as a clip's shape, in the space that `fromDevice` maps target pixels to.
void cornerShape(const PrCornerRect& shape, PrAffine2 fromDevice, FragmentClipShape* out) {
    *out = FragmentClipShape{};
    out->shape = recordOf(fittedCorners(shape));
    out->transform = prMatrix3FromAffine2(fromDevice);
}

// A shape that an instance evaluates, in its own space, which `toDevice` maps to target pixels.
struct LocalShape : InstanceCornerRect {
    PrAffine2 toDevice;
};

// Writes the shape that a command fills, if an instance can evaluate it. Returns false for any other command.
bool localShapeOf(const Walk& w, const PrDrawCommand& c, LocalShape* out) {
    const PrDrawTransform& x = w.transforms.last();
    *out = LocalShape{};
    out->toDevice = x.affine;
    switch (c.kind) {
        case PrDrawCommandKind_Rect:
            out->rect = c.rect;
            return true;
        case PrDrawCommandKind_CornerRect:
            static_cast<InstanceCornerRect&>(*out) = fittedCorners(*c.cornerRect);
            return true;
        case PrDrawCommandKind_Capsule: {
            // The capsule's own frame runs from its start along its axis. The capsule is a rounded rectangle with the
            // capsule's radius.
            PrVec2 d{c.capsule.end.x - c.capsule.start.x, c.capsule.end.y - c.capsule.start.y};
            float length = prF32Sqrt(d.x * d.x + d.y * d.y);
            float r = c.capsule.radius;
            if (!(r > 0))
                return false;
            out->toDevice = prAffine2Concat(prAffine2Concat(prAffine2Rotate(prF32Atan2(d.y, d.x)),
                prAffine2Translate(c.capsule.start)), x.affine);
            out->rect = PrRect{-r, -r, length + r, r};
            for (float& radius : out->radii)
                radius = r;
            return true;
        }
        case PrDrawCommandKind_Ellipse: {
            // The ellipse's own frame is turned by its rotation about its center.
            PrVec2 r = c.ellipse.radii;
            out->toDevice = prAffine2Concat(prAffine2Concat(prAffine2Rotate(c.ellipse.rotation),
                prAffine2Translate(c.ellipse.center)), x.affine);
            out->rect = PrRect{-r.x, -r.y, r.x, r.y};
            PrCorner round = prCornerRound(r);
            return fittedRadii(PrCornerRect{out->rect, {round, round, round, round}}, out->radii);
        }
        default:
            return false;
    }
}

// Draws a local shape with the ink. A rectangle that stays axis-aligned is drawn as the area of each pixel that it
// covers. Any other shape is drawn as the rounded rectangle's coverage.
void drawLocalShape(Walk& w, const LocalShape& shape, const Ink& ink) {
    bool square = true;
    for (float radius : shape.radii)
        square = square && radius <= 0;
    if (square && prAffine2IsAxisAligned(shape.toDevice)) {
        PrRect device = prAffine2MapRect(shape.toDevice, shape.rect);
        PrIRect box = prIRectIntersect(prIRectRoundOut(device), clipOf(w).box);
        if (prIRectIsEmpty(box))
            return;
        if (prRectIsOnPixels(device)) {
            emit(w, boxInstance(box, FragmentCoverage_Solid), ink, box);
            return;
        }
        FragmentInstance i = boxInstance(box, FragmentCoverage_Rect);
        i.data = w.b->values.count;
        float values[4] = {device.left, device.top, device.right, device.bottom};
        if (emit(w, i, ink, box))
            w.b->values.append(values, 4);
        return;
    }
    drawShaped(w, FragmentCoverage_Shape, shape.toDevice, grownByPixels(shape.rect, shape.toDevice, 1), recordOf(shape), ink);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rings

// Draws the outer rounded rectangle less the inner one, if there is one, within the three half planes, or outside them.
void drawRing(Walk& w, PrAffine2 toDevice, const InstanceCornerRect& outer, const InstanceCornerRect* PR_NULLABLE inner,
        const float planes[9], bool outside, const Ink& ink) {
    FragmentRing ring{};
    ring.outer = recordOf(outer);
    // Without an inner rectangle, the record uses one far away, which covers nothing.
    ring.inner = recordOf(inner != nullptr ? *inner : InstanceCornerRect{PrRect{1e9f, 1e9f, 1e9f, 1e9f}, {}});
    prMemCopy(ring.planes, planes, sizeof(ring.planes));
    ring.outside = outside ? 1 : 0;
    drawShaped(w, FragmentCoverage_Ring, toDevice, grownByPixels(outer.rect, toDevice, 1), ring, ink);
}

// Returns the rounded rectangle offset by `by`, outward if `by` is positive. A radius grows or shrinks with the offset,
// to no less than zero. A square corner stays square, or rounds by the offset outward if `round`.
InstanceCornerRect offsetRounded(const InstanceCornerRect& shape, float by, bool round) {
    const PrRect& r = shape.rect;
    InstanceCornerRect out{PrRect{r.left - by, r.top - by, r.right + by, r.bottom + by}, {}};
    for (uint32_t i = 0; i < 8; ++i)
        out.radii[i] = shape.radii[i] > 0 ? prF32Max(shape.radii[i] + by, 0) : round && by > 0 ? by : 0;
    return out;
}

// Draws a border whose sides color one ring (prDrawBorderSides) as one instance whose pixels take the color of the side
// that they lie on (FragmentSidedRing). Returns false for any other border.
bool drawSidedRing(Walk& w, const PrDrawCommand& c) {
    const PrDrawBorder& border = *c.border;
    PrDrawBorderSides sides{};
    if (!prDrawBorderSides(&border, &sides))
        return false;
    FragmentSidedRing ring{};
    FragmentPaint paint{};
    for (uint32_t k = 0; k < 4; ++k) {
        for (uint32_t j = 0; j < 2; ++j)
            makePaint(w, nullptr, sides.colors[k][j], prDrawFillBounds(&c), &paint, ring.colors[k][j]);
    }
    prMemCopy(ring.divides, sides.lines, sizeof(ring.divides));
    ring.split = sides.split;
    InstanceCornerRect outer = fittedCorners(prDrawBorderContour(&border, 0));
    PrCornerRect within = prDrawBorderContour(&border, 1);
    ring.outer = recordOf(outer);
    // Without an inner rectangle, the record uses one far away, which covers nothing.
    InstanceCornerRect inner = InstanceCornerRect{PrRect{1e9f, 1e9f, 1e9f, 1e9f}, {}};
    if (!prRectIsEmpty(within.rect))
        inner = fittedCorners(within);
    ring.inner = recordOf(inner);
    const PrAffine2& toDevice = w.transforms.last().affine;
    drawShaped(w, FragmentCoverage_SidedRing, toDevice, grownByPixels(outer.rect, toDevice, 1), ring,
        inkOf(w, paint, ring.colors[0][1]));
    return true;
}

// Draws a border whose sides fill bands, with round or square corners, as rings. A uniform border has one ring for each
// band of its single area. A border whose sides that paint are each one band of a color is one ring that colors each
// side (drawSidedRing). Any other border has one ring for each band of each side, cut to the side's wedge. Returns
// false for any other kind of border, which draws nothing here.
bool drawBorderRings(Walk& w, const PrDrawCommand& c) {
    if (drawSidedRing(w, c))
        return true;
    const PrDrawTransform& x = w.transforms.last();
    const PrDrawBorder& border = *c.border;
    PrRect rect = border.shape.rect;
    // Computes the contours of every band first, so that a border that they cannot describe draws nothing here.
    struct Piece {
        uint32_t side;
        PrDrawBorderBand band;
        InstanceCornerRect outer;
        InstanceCornerRect inner;
        bool hasInner;
    };
    Piece pieces[8];
    uint32_t count = 0;
    const PrDrawBorderSide* uniform = prDrawBorderUniformSide(&border);
    for (uint32_t side = 0; side < 4; ++side) {
        const PrDrawBorderSide& s = border.sides[side];
        bool paints = s.width > 0 && prDrawBorderStylePaints(s.style);
        if (paints && (s.style == PrDrawBorderStyle_Dashed || s.style == PrDrawBorderStyle_Dotted))
            return false;
        PrDrawBorderBand bands[kPrDrawBorderMaxBands];
        uint32_t n = prDrawBorderBands(&border, PrRectSide(side), bands);
        if (uniform && side != 0)
            break;
        if (uniform) {
            // One area with the style and color that every side that paints shares, whatever the first side's width.
            bool twice = uniform->style == PrDrawBorderStyle_Double;
            bands[0] = PrDrawBorderBand{0, twice ? 1.0f / 3 : 1, uniform->color};
            bands[1] = PrDrawBorderBand{2.0f / 3, 1, uniform->color};
            n = twice ? 2 : 1;
        }
        for (uint32_t i = 0; i < n; ++i) {
            Piece& p = pieces[count];
            p.side = side;
            p.band = bands[i];
            PrCornerRect contour = prDrawBorderContour(&border, bands[i].from);
            if (prRectIsEmpty(contour.rect))
                continue;
            p.outer = fittedCorners(contour);
            contour = prDrawBorderContour(&border, bands[i].to);
            p.hasInner = !prRectIsEmpty(contour.rect);
            p.inner = p.hasInner ? fittedCorners(contour) : InstanceCornerRect{contour.rect, {}};
            ++count;
        }
    }
    // The lines that divide the sides, from the corners of the outer rectangle to the corners of the inner one.
    float widths[4];
    for (uint32_t side = 0; side < 4; ++side) {
        const PrDrawBorderSide& s = border.sides[side];
        widths[side] = s.width > 0 && prDrawBorderStylePaints(s.style) ? s.width : 0;
    }
    PrRect inner{
        rect.left + widths[PrRectSide_Left], rect.top + widths[PrRectSide_Top],
        rect.right - widths[PrRectSide_Right], rect.bottom - widths[PrRectSide_Bottom],
    };
    // Where the bands of opposite sides part. Each side's band reaches inward by its width or by its corners' radii,
    // whichever is greater. Bands that overlap there draw nothing here.
    const PrCornerRect fittedShape = prCornerRectFit(border.shape);
    const PrCorner* fitted = fittedShape.corners;
    float topReach = prF32Max(widths[PrRectSide_Top], prF32Max(fitted[0].radii.y, fitted[1].radii.y));
    float bottomReach = prF32Max(widths[PrRectSide_Bottom], prF32Max(fitted[2].radii.y, fitted[3].radii.y));
    float leftReach = prF32Max(widths[PrRectSide_Left], prF32Max(fitted[0].radii.x, fitted[3].radii.x));
    float rightReach = prF32Max(widths[PrRectSide_Right], prF32Max(fitted[1].radii.x, fitted[2].radii.x));
    bool overlap = rect.top + topReach > rect.bottom - bottomReach || rect.left + leftReach > rect.right - rightReach;
    if (!uniform && overlap)
        return false;
    float splitY = (rect.top + topReach + rect.bottom - bottomReach) / 2;
    float splitX = (rect.left + leftReach + rect.right - rightReach) / 2;
    for (uint32_t i = 0; i < count; ++i) {
        const Piece& p = pieces[i];
        const PrDrawBorderSide& s = border.sides[p.side];
        FragmentPaint r{};
        float color[4];
        if (!makePaint(w, s.brush, p.band.color, prDrawFillBounds(&c), &r, color))
            continue;
        float planes[9] = {};
        if (!uniform) {
            // The half plane that holds the side's part of the ring.
            float half[4][3] = {{0, 1, -splitY}, {-1, 0, splitX}, {0, -1, splitY}, {1, 0, -splitX}};
            prMemCopy(planes + 6, half[p.side], sizeof(half[p.side]));
            PrVec2 q0 = prRectCornerPoint(rect, PrRectCorner(p.side));
            PrVec2 q1 = prRectCornerPoint(rect, PrRectCorner((p.side + 1) % 4));
            PrVec2 middle{(q0.x + q1.x) / 2, (q0.y + q1.y) / 2};
            for (uint32_t k = 0; k < 2; ++k) {
                uint32_t at = (p.side + k) % 4;
                PrVec2 q = prRectCornerPoint(rect, PrRectCorner(at));
                PrVec2 d = prRectCornerPoint(inner, PrRectCorner(at)) - q;
                float a = -d.y;
                float b = d.x;
                float offset = -(a * q.x + b * q.y);
                // Orients the plane so that the side's outer edge lies inside.
                if (a * middle.x + b * middle.y + offset > 0) {
                    a = -a;
                    b = -b;
                    offset = -offset;
                }
                planes[k * 3] = a;
                planes[k * 3 + 1] = b;
                planes[k * 3 + 2] = offset;
            }
        }
        drawRing(w, x.affine, p.outer, p.hasInner ? &p.inner : nullptr, planes, false, inkOf(w, r, color));
    }
    return true;
}

// A stroke as a ring: the transform of its shape's space, its outer and inner rounded rectangles, and how much its
// paint fades.
struct StrokeRing {
    PrAffine2 toDevice;
    InstanceCornerRect outer;
    InstanceCornerRect inner;
    bool hasInner;
    float fade;
};

// Computes the stroke of a rectangle, rounded rectangle or capsule with circular corners, or of a circle, as a ring.
// Returns false for any other stroke: one that is dashed, has a width profile, has bevel joins or miters that do not
// reach, or is of another shape.
bool strokeRingOf(const Walk& w, const PrDrawCommand& c, StrokeRing* out) {
    const PrDrawStroke& stroke = *c.stroke;
    const PrPathStroke& s = stroke.shape;
    LocalShape shape{};
    if (!c.antialias || !(s.width > 0) || (s.dashes != nullptr && s.dashCount != 0) ||
            (s.profile != nullptr && s.profileCount != 0) || !localShapeOf(w, c, &shape))
        return false;
    // Offsetting keeps the shape of a circular corner, and of no other corner.
    bool square = false;
    for (uint32_t k = 0; k < 4; ++k) {
        float rx = shape.radii[k * 2];
        float ry = shape.radii[k * 2 + 1];
        if (rx != ry || (rx > 0 && shape.corners[k] != 1))
            return false;
        square = square || rx <= 0;
    }
    bool round = s.join == PrPathJoin_Round;
    if (square && !round && !(s.join == PrPathJoin_Miter && s.miterLimit >= 1.4143f))
        return false;
    // The width in the shape's space, and on the device.
    float scale = prAffine2MaxScale(shape.toDevice);
    if (!(scale > 0))
        return false;
    bool device = stroke.nonScaling;
    if (device && !prAffine2IsSimilarity(shape.toDevice))
        return false;
    float width = stroke.nonScaling ? s.width / scale : s.width;
    out->fade = 1;
    if (width * scale < 1) {
        out->fade = width * scale;
        width = 1 / scale;
    }
    float outward =
        stroke.align == PrDrawStrokeAlign_Inside    ? 0 : stroke.align == PrDrawStrokeAlign_Outside ? width : width / 2;
    float inward = width - outward;
    const InstanceCornerRect& base = shape;
    out->toDevice = shape.toDevice;
    out->outer = offsetRounded(base, outward, round);
    out->inner = offsetRounded(base, -inward, false);
    out->hasInner = !prRectIsEmpty(out->inner.rect);
    return true;
}

// Computes the stroke of a line with the same cap at both ends, as a rectangle or rounded rectangle along the line.
// Butt caps end at the line's ends, square caps extend half the width past them, and round caps are half circles.
// Returns false for any other line.
bool lineShapeOf(const Walk& w, const PrDrawCommand& c, LocalShape* out, float* fade) {
    const PrDrawTransform& x = w.transforms.last();
    const PrPathStroke& s = c.stroke->shape;
    if (!c.antialias || !(s.width > 0) || (s.dashes != nullptr && s.dashCount != 0) ||
            (s.profile != nullptr && s.profileCount != 0) || s.startCap != s.endCap || s.startCap == PrPathCap_Triangle)
        return false;
    PrVec2 d{c.line.end.x - c.line.start.x, c.line.end.y - c.line.start.y};
    float length = prF32Sqrt(d.x * d.x + d.y * d.y);
    if (!(length > 0))
        return false;
    *out = LocalShape{};
    out->toDevice = prAffine2Concat(prAffine2Concat(prAffine2Rotate(prF32Atan2(d.y, d.x)),
        prAffine2Translate(c.line.start)), x.affine);
    float scale = prAffine2MaxScale(out->toDevice);
    bool device = c.stroke->nonScaling;
    if (!(scale > 0) || (device && !prAffine2IsSimilarity(out->toDevice)))
        return false;
    float width = c.stroke->nonScaling ? s.width / scale : s.width;
    *fade = 1;
    if (width * scale < 1) {
        *fade = width * scale;
        width = 1 / scale;
    }
    float half = width / 2;
    float past = s.startCap == PrPathCap_Butt ? 0 : half;
    out->rect = PrRect{-past, -half, length + past, half};
    if (s.startCap == PrPathCap_Round) {
        for (float& radius : out->radii)
            radius = half;
    }
    return true;
}

// Writes the shape that a command clips to, if a clip record can evaluate it, and its device bounds. Returns false for
// any other command.
bool clipShapeOf(const Walk& w, const PrDrawCommand& c, FragmentClipShape* out, PrRect* bounds) {
    const PrDrawTransform& x = w.transforms.last();
    PrAffine2 fromDevice{};
    if (c.kind == PrDrawCommandKind_PushClipTargetRect) {
        *bounds = c.rect;
        cornerShape(PrCornerRect{c.rect, {}}, prAffine2Identity(), out);
        return true;
    }
    if (!prAffine2Invert(x.affine, &fromDevice))
        return false;
    switch (c.kind) {
        case PrDrawCommandKind_Rect:
        case PrDrawCommandKind_PushClipRect:
            *bounds = prAffine2MapRect(x.affine, c.rect);
            cornerShape(PrCornerRect{c.rect, {}}, fromDevice, out);
            return true;
        case PrDrawCommandKind_CornerRect:
        case PrDrawCommandKind_PushClipCornerRect:
            *bounds = prAffine2MapRect(x.affine, c.cornerRect->rect);
            cornerShape(*c.cornerRect, fromDevice, out);
            return true;
        case PrDrawCommandKind_Ellipse: {
            // The ellipse's own frame is centered on the ellipse and turned back by its rotation.
            PrVec2 r = c.ellipse.radii;
            PrAffine2 local = prAffine2Concat(prAffine2Translate(-c.ellipse.center),
                prAffine2Rotate(-c.ellipse.rotation));
            PrCorner round = prCornerRound(r);
            PrCornerRect rect{PrRect{-r.x, -r.y, r.x, r.y}, {round, round, round, round}};
            prPathClear(w.f->region);
            prDrawShapePath(&c, w.f->region);
            *bounds = prAffine2MapRect(x.affine, prPathBounds(w.f->region));
            cornerShape(rect, prAffine2Concat(fromDevice, local), out);
            return true;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Lists

void pushClip(Walk& w, const PrDrawCommand& c);
void popClip(Walk& w);

// ---------------------------------------------------------------------------------------------------------------------
// Commands

// Bins the map's segments into its grid's cells in the frame's map indices.
void binSegments(Walk& w, FragmentMap& m, uint32_t segmentCount, PrRect bounds) {
    float extentX = prRectWidth(bounds) + 2 * m.reach;
    float extentY = prRectHeight(bounds) + 2 * m.reach;
    m.origin = PrVec2{bounds.left - m.reach, bounds.top - m.reach};
    m.cellSize = prF32Max(kMapCell, prF32Max(extentX, extentY) / float(kMapCells));
    m.columns = uint32_t(prF32Max(prF32Ceil(extentX / m.cellSize), 1));
    m.rows = uint32_t(prF32Max(prF32Ceil(extentY / m.cellSize), 1));
    uint32_t cells = m.columns * m.rows;
    RawArray<uint32_t>& indices = w.b->mapIndices;
    m.cells = indices.count;
    indices.reserve(indices.count + cells + 1);
    indices.count += cells + 1;
    uint32_t* starts = indices.data + m.cells;
    prMemZero(starts, (cells + 1) * sizeof(uint32_t));
    // The cells that a segment's reach overlaps.
    auto each = [&](uint32_t k, auto visit) {
        const float* g = w.b->mapData.data + m.segments + k * 6;
        float left = (prF32Min(g[0], g[2]) - m.reach - m.origin.x) / m.cellSize;
        float top = (prF32Min(g[1], g[3]) - m.reach - m.origin.y) / m.cellSize;
        float right = (prF32Max(g[0], g[2]) + m.reach - m.origin.x) / m.cellSize;
        float bottom = (prF32Max(g[1], g[3]) + m.reach - m.origin.y) / m.cellSize;
        uint32_t x0 = uint32_t(prF32Clamp(prF32Floor(left), 0, float(m.columns - 1)));
        uint32_t x1 = uint32_t(prF32Clamp(prF32Floor(right), 0, float(m.columns - 1)));
        uint32_t y0 = uint32_t(prF32Clamp(prF32Floor(top), 0, float(m.rows - 1)));
        uint32_t y1 = uint32_t(prF32Clamp(prF32Floor(bottom), 0, float(m.rows - 1)));
        for (uint32_t y = y0; y <= y1; ++y) {
            for (uint32_t x = x0; x <= x1; ++x)
                visit(y * m.columns + x);
        }
    };
    for (uint32_t k = 0; k < segmentCount; ++k)
        each(k, [&](uint32_t cell) { ++starts[cell + 1]; });
    uint32_t base = indices.count;
    starts[0] = base;
    for (uint32_t cell = 0; cell < cells; ++cell)
        starts[cell + 1] += starts[cell];
    uint32_t total = starts[cells] - base;
    RawArray<uint32_t>& cursors = w.f->cursors;
    cursors.clear();
    cursors.append(starts, cells);
    indices.reserve(indices.count + total);
    indices.count += total;
    for (uint32_t k = 0; k < segmentCount; ++k)
        each(k, [&](uint32_t cell) { w.b->mapIndices.data[cursors.data[cell]++] = k; });
}

// A mapped stroke's map, the transform from its unit square to brush space, and the number of target pixels that one
// unit of the square covers along each axis.
struct StrokeMapping {
    uint32_t map;
    PrAffine2 unitToBrush;
    PrVec2 unitPixels;
};

// Measures the centerline of a stroke whose brush is mapped along or across it into a map of the frame, as
// draw_fragment.h describes, and places the record's brush by it. Returns false if it paints nothing.
bool mapStroke(Walk& w, const PrDrawCommand& c, StrokeMapping* out) {
    Fragment* f = w.f;
    const PrDrawTransform& x = w.transforms.last();
    const PrDrawStroke& stroke = *c.stroke;
    PrAffine2 unitToBrush{};
    if (!prAffine2Invert(c.brush->transform, &unitToBrush))
        return false;
    prPathClear(f->shape);
    prDrawShapePath(&c, f->shape);
    const PrPathPolyline& centerline = land(w, f->shape, kSparseTolerance, PrPathFlattening_Inside, f->centerline);
    float scale = stroke.nonScaling ? 1 : prAffine2MaxScale(x.affine);
    FragmentMap m{};
    m.mapping = stroke.mapping;
    m.width = stroke.shape.width * scale;
    // A stroke narrower than a pixel is drawn a pixel wide.
    m.reach = prDrawStrokeReach(&stroke) * prF32Max(stroke.shape.width * scale, 1) + 2;
    RawArray<float>& data = w.b->mapData;
    m.segments = data.count;
    PrRect bounds{kPrF32Infinity, kPrF32Infinity, -kPrF32Infinity, -kPrF32Infinity};
    float longest = 0;
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
            length += prF32Sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        }
        if (count < 2)
            continue;
        longest = prF32Max(longest, length);
        if (!(length > 0))
            continue;
        float along = 0;
        for (uint32_t i = 0; i < segments; ++i) {
            PrVec2 a = points[i];
            PrVec2 b = points[(i + 1) % count];
            float span = prF32Sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            if (!(span > 0))
                continue;
            float segment[6] = {a.x, a.y, b.x, b.y, along, length};
            data.append(segment, 6);
            bounds = PrRect{
                prF32Min(bounds.left, prF32Min(a.x, b.x)), prF32Min(bounds.top, prF32Min(a.y, b.y)),
                prF32Max(bounds.right, prF32Max(a.x, b.x)), prF32Max(bounds.bottom, prF32Max(a.y, b.y)),
            };
            along += span;
        }
    }
    uint32_t segmentCount = (data.count - m.segments) / 6;
    if (segmentCount == 0) {
        data.count = m.segments;
        return false;
    }
    m.profile = data.count;
    m.profileCount = stroke.shape.profile != nullptr ? stroke.shape.profileCount : 0;
    for (uint32_t i = 0; i < m.profileCount; ++i) {
        const PrPathWidthPoint& point = stroke.shape.profile[i];
        float values[3] = {point.position, point.left, point.right};
        data.append(values, 3);
    }
    binSegments(w, m, segmentCount, bounds);
    float across = prF32Max(stroke.shape.width * scale, 1);
    uint32_t map = w.b->maps.count + 1;
    w.b->maps.push(m);
    *out = StrokeMapping{
        map, unitToBrush,
        stroke.mapping == PrDrawStrokeMapping_Along ? PrVec2{longest, across} : PrVec2{across, longest},
    };
    return true;
}

// Returns true if the command strokes with a brush mapped along or across the stroke.
bool mapsStroke(const PrDrawCommand& c) {
    return c.stroke != nullptr && c.brush != nullptr && c.stroke->mapping != PrDrawStrokeMapping_Plain;
}

bool drawAtlasPath(Walk& w, const PrDrawCommand& c, const Ink& ink);

// Draws a shape. A filled shape that an instance evaluates is one instance. A path drawn without rotation or skew at a
// size that the atlas holds uses the atlas's coverage. Anything else is drawn as a path.
void drawShape(Walk& w, const PrDrawCommand& c) {
    if (c.kind == PrDrawCommandKind_Line && c.stroke == nullptr)
        return;
    StrokeMapping mapping{};
    bool mapped = mapsStroke(c);
    if (mapped && !mapStroke(w, c, &mapping))
        return;
    FragmentPaint r{};
    float color[4];
    if (!makePaint(w, c.brush, c.color, prDrawFillBounds(&c), &r, color, mapped ? &mapping.unitPixels : nullptr))
        return;
    LocalShape shape{};
    if (c.stroke == nullptr && c.antialias && localShapeOf(w, c, &shape)) {
        drawLocalShape(w, shape, inkOf(w, r, color));
        return;
    }
    StrokeRing ring{};
    float fade = 1;
    if (c.stroke != nullptr && !mapped && c.kind == PrDrawCommandKind_Line && lineShapeOf(w, c, &shape, &fade)) {
        drawLocalShape(w, shape, fadedInk(inkOf(w, r, color), fade));
        return;
    }
    if (c.stroke != nullptr && !mapped && c.kind != PrDrawCommandKind_Line && strokeRingOf(w, c, &ring)) {
        float none[9] = {};
        drawRing(w, ring.toDevice, ring.outer, ring.hasInner ? &ring.inner : nullptr, none, false,
            fadedInk(inkOf(w, r, color), ring.fade));
        return;
    }
    if (mapped) {
        r.map = mapping.map;
        r.brush = prMatrix3FromAffine2(mapping.unitToBrush);
    }
    Ink ink = inkOf(w, r, color);
    if (!mapped && drawAtlasPath(w, c, ink))
        return;
    prPathClear(w.f->shape);
    prDrawShapePath(&c, w.f->shape);
    if (c.stroke != nullptr)
        strokePath(w, w.f->shape, c.stroke, c.antialias, ink);
    else
        fillPath(w, w.f->shape, c.rule, c.antialias, ink);
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
    FragmentPaint r{};
    float color[4];
    if (!makePaint(w, c.border->sides[piece->side].brush, piece->color, prDrawFillBounds(&c), &r, color))
        return;
    Ink ink = inkOf(w, r, color);
    const PrDrawTransform& x = w.transforms.last();
    prPathPolylineClear(&w.f->polyline);
    prPathPolylineAddPolyline(&w.f->polyline, shape, x.affine);
    if (cut == nullptr) {
        fillDevice(w, w.f->polyline, piece->rule, c.antialias, ink);
        return;
    }
    // The piece and its cut are combined in target pixels.
    prPathPolylineClear(&w.f->cut);
    prPathPolylineAddPolyline(&w.f->cut, cut, x.affine);
    prPathPolylineClear(&w.f->combined);
    prPathPolylineCombine(&w.f->polyline, piece->rule, &w.f->cut, PrPathFillRule_NonZero, PrPathPolylineOp_Intersect,
        &w.f->combined);
    fillDevice(w, w.f->combined, PrPathFillRule_NonZero, c.antialias, ink);
}

void drawBorder(Walk& w, const PrDrawCommand& c) {
    float scale = prAffine2MaxScale(w.transforms.last().affine);
    if (!(scale > 0))
        return;
    if (drawBorderRings(w, c))
        return;
    BorderDraw d{&w, &c};
    prDrawBorderLower(c.border, toleranceOf(w, c.antialias) / scale, drawBorderPiece, &d);
}

void drawDecoration(Walk& w, const PrDrawCommand& c) {
    FragmentPaint r{};
    float color[4];
    if (!makePaint(w, c.brush, c.color, prDrawFillBounds(&c), &r, color))
        return;
    Ink ink = inkOf(w, r, color);
    const PrDrawTransform& x = w.transforms.last();
    float scale = prAffine2MaxScale(x.affine);
    if (!(scale > 0))
        return;
    prPathPolylineClear(&w.f->cut);
    prDrawDecorationOutline(c.decoration, toleranceOf(w, c.antialias) / scale, &w.f->cut);
    prPathPolylineClear(&w.f->polyline);
    prPathPolylineAddPolyline(&w.f->polyline, &w.f->cut, x.affine);
    fillDevice(w, w.f->polyline, PrPathFillRule_NonZero, c.antialias, ink);
}

// Replaces what the clip lets through with the color.
void drawClear(Walk& w, const PrDrawCommand& c) {
    PrIRect box = prIRectIntersect(clipOf(w).box, targetBox(w));
    FragmentPaint r{};
    float color[4];
    if (prIRectIsEmpty(box))
        return;
    makePaint(w, nullptr, c.color, PrRect{}, &r, color);
    // If the target is not a knockout group, copying an opaque color is the same as drawing it over what lies beneath.
    bool opaque = color[3] >= 1 && r.knockout == 0;
    r.composite = opaque ? PrDrawComposite_SourceOver : PrDrawComposite_Copy;
    r.blendMode = PrDrawBlendMode_Normal;
    drawSolid(w, box, inkOf(w, r, color));
}

// Draws a shadow as one instance in the shadow's space, which blurs its shape by the shadow's sigma. A drop shadow cuts
// out its caster, and an inner shadow blurs the outside of its shape within its opening.
void drawShadow(Walk& w, const PrDrawCommand& c) {
    const PrDrawTransform& x = w.transforms.last();
    const PrDrawShadowKind kind =
        c.kind == PrDrawCommandKind_InnerShadow ? PrDrawShadowKind_Inner : PrDrawShadowKind_Drop;
    PrDrawShadowGeometry g = prDrawShadowGeometry(c.shadow, kind);
    PrAffine2 device = x.affine;
    float scale = prAffine2MaxScale(device);
    PrAffine2 shifted = device;
    if (c.shadow->nonScaling)
        shifted = prAffine2Concat(device, prAffine2Translate(c.shadow->offset));
    PrAffine2 shadowFromDevice{};
    if (!(scale > 0) || !prAffine2Invert(shifted, &shadowFromDevice))
        return;
    FragmentPaint r{};
    float color[4];
    if (!makePaint(w, c.brush, c.color, prDrawFillBounds(&c), &r, color))
        return;
    // The caster in the shadow's space, which differs from the caster's own space by a translation.
    PrRect caster = prAffine2MapRect(prAffine2Concat(device, shadowFromDevice), c.shadow->shape.rect);
    const PrRect& shape = g.shape.rect;
    InstanceCornerRect shapeCorners{shape, {}};
    InstanceCornerRect casterCorners{caster, {}};
    if (!fittedRadii(g.shape, shapeCorners.radii) || !fittedRadii(c.shadow->shape, casterCorners.radii))
        return;
    FragmentShadow values{};
    values.shape = recordOf(shapeCorners);
    values.caster = recordOf(casterCorners);
    // A blur narrower than a pixel is replaced by the shape's own edge.
    float sigma = c.shadow->nonScaling ? g.sigma / scale : g.sigma;
    values.sigma = sigma * scale < 0.5f ? 0 : sigma;
    values.inner = kind == PrDrawShadowKind_Inner ? 1 : 0;
    float reach = kShadowReach * sigma;
    PrRect bounds;
    if (kind == PrDrawShadowKind_Inner)
        bounds = caster;
    else
        bounds = PrRect{shape.left - reach, shape.top - reach, shape.right + reach, shape.bottom + reach};
    drawShaped(w, FragmentCoverage_Shadow, shifted, grownByPixels(bounds, shifted, 1), values, inkOf(w, r, color));
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Draws a nine-slice. Its pieces draw as one shape, so that no edge between two of them is antialiased. Each pixel
// samples the piece that it falls in. A list source renders at the largest scale of any piece.
void drawNineSlice(Walk& w, const PrDrawCommand& c) {
    const PrDrawSource& source = c.nineSlice.source;
    const PrRect& area = source.rect;
    const PrDrawTransform& x = w.transforms.last();
    uint32_t count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{prRectWidth(area), prRectHeight(area)},
        c.nineSlice.destination, nullptr, 0);
    if (count == 0 || prIRectIsEmpty(clipOf(w).box))
        return;
    RawArray<PrDrawImageRect> rects;
    rects.reserve(count);
    rects.count = prDrawNineSliceRects(c.nineSlice.slices, PrVec2{prRectWidth(area), prRectHeight(area)},
        c.nineSlice.destination, rects.data, count);
    float most = 0;
    for (uint32_t i = 0; i < rects.count; ++i) {
        const PrDrawImageRect& p = rects.data[i];
        most = prF32Max(most, prRectWidth(p.destination) / prF32Max(prRectWidth(p.source), 1e-6f));
        most = prF32Max(most, prRectHeight(p.destination) / prF32Max(prRectHeight(p.source), 1e-6f));
    }
    float device = prAffine2MaxScale(x.affine);
    SourceTexture texture{};
    FragmentPaint r{};
    float color[4];
    // Maps target pixels to the command's space, which holds the pieces.
    PrAffine2 inverse{};
    bool placed = prAffine2Invert(x.affine, &inverse);
    PrMatrix3 fromDevice = prMatrix3FromAffine2(inverse);
    if (placed && sourceTexture(w, source, area, PrVec2{device * most, device * most}, c.sampling, &texture) &&
            makePaint(w, nullptr, c.color, c.nineSlice.destination, &r, color)) {
        r.brush = fromDevice;
        r.kind = FragmentPaintKind_Image;
        r.sampling = c.sampling & PrDrawSamplingFlag_Kind;
        // Each piece clamps to its texels, which pad as a tile does.
        PrDrawTiles padded{.tile = {0, 0, 1, 1}, .x = PrDrawExtend_Pad, .y = PrDrawExtend_Pad};
        r.read = imageRead(c.sampling, texture.alpha, padded, PrRect{0, 0, 0, 0}, texture.width, texture.height);
        r.alpha = texture.alpha;
        r.pieces = w.b->pieces.count;
        r.pieceCount = rects.count;
        r.texture = tableOfSource(w, texture);
        // The sources of the pieces, in the texture's texels.
        float kx = prRectWidth(texture.texels) / prRectWidth(area);
        float ky = prRectHeight(texture.texels) / prRectHeight(area);
        prPathClear(w.f->shape);
        for (uint32_t i = 0; i < rects.count; ++i) {
            const PrDrawImageRect& piece = rects.data[i];
            const PrRect& d = piece.destination;
            const PrRect& t = texture.texels;
            FragmentPiece p{
                {d.left, d.top, d.right, d.bottom},
                {
                    t.left + piece.source.left * kx, t.top + piece.source.top * ky,
                    t.left + piece.source.right * kx, t.top + piece.source.bottom * ky,
                },
            };
            w.b->pieces.push(p);
            prPathAddRect(w.f->shape, d);
        }
        fillPath(w, w.f->shape, PrPathFillRule_NonZero, c.antialias, inkOf(w, r, color));
    }
    rects.reset();
}

// Draws the image's texels at a device point, over the target regardless of the clip. The texels are blended instead of
// replacing the target's pixels.
void writePixels(Walk& w, const PrDrawCommand& c) {
    const PrDrawImage* image = c.writePixels.image;
    uint32_t texture = textureOfImage(w, image);
    PrIRect into = targetBox(w);
    if (texture == 0 || prIRectIsEmpty(into))
        return;
    float width = float(prDrawImageExtent(image).x);
    float height = float(prDrawImageExtent(image).y);
    FragmentPaint r{};
    r.brush = prMatrix3FromAffine2(prAffine2Identity());
    r.kind = FragmentPaintKind_Image;
    r.sampling = PrDrawSampling_Nearest;
    r.read = FragmentImageRead_Sampler;
    r.tileX = PrDrawExtend_None;
    r.tileY = PrDrawExtend_None;
    r.tile = prRectFromIRect(prIRectAt(c.writePixels.origin, prDrawImageExtent(image)));
    r.texels = PrRect{0, 0, width, height};
    r.texture = tableOf(w, texture);
    float white[4] = {1, 1, 1, 1};
    Ink ink = inkOf(w, r, white);
    Clip everywhere{};
    everywhere.box = into;
    everywhere.rect = PrRect{float(into.left), float(into.top), float(into.right), float(into.bottom)};
    everywhere.whole = true;
    w.clips.push(everywhere);
    PrIRect box{
        c.writePixels.origin.x, c.writePixels.origin.y, c.writePixels.origin.x + int32_t(width),
        c.writePixels.origin.y + int32_t(height),
    };
    drawSolid(w, box, ink);
    w.clips.pop();
}

// ---------------------------------------------------------------------------------------------------------------------
// Glyphs

// Returns the texture of an atlas page. The texture is created at the end of the build.
uint32_t pageTexture(Walk& w, uint32_t page) {
    Fragment* f = w.f;
    while (f->pageTextures.count <= page)
        f->pageTextures.push(++f->nextTexture);
    return f->pageTextures.data[page];
}

// Draws the atlas's entry with the ink.
void drawAtlasEntry(Walk& w, const PrDrawAtlasEntry& e, const Ink& ink) {
    PrIRect box{e.origin.x, e.origin.y, e.origin.x + int32_t(e.extent.x), e.origin.y + int32_t(e.extent.y)};
    if (e.extent.x == 0 || prIRectIsEmpty(prIRectIntersect(box, clipOf(w).box)))
        return;
    // Local space is the page's texels.
    FragmentInstance i = boxInstance(box, FragmentCoverage_Atlas);
    i.transform = prAffine2Translate(PrVec2{float(box.left) - float(e.texel.x), float(box.top) - float(e.texel.y)});
    i.bounds = PrRect{float(e.texel.x), float(e.texel.y), float(e.texel.x + e.extent.x), float(e.texel.y + e.extent.y)};
    i.data = tableOf(w, pageTexture(w, e.page));
    emit(w, i, ink, box);
}

// Draws the glyph from the atlas if it is drawn without rotation or skew at a size that the atlas holds. Returns false
// if the glyph must be filled as a path instead.
bool drawAtlasGlyph(Walk& w, const PrDrawCommand& c, uint32_t index, const Ink& ink) {
    const PrDrawTransform& x = w.transforms.last();
    if (!c.antialias)
        return false;
    PrDrawAtlasEntry e{};
    if (!prDrawAtlasGlyph(w.f->atlas, c.glyphs, index, x.affine, &e))
        return false;
    drawAtlasEntry(w, e, ink);
    return true;
}

// Draws a Path command from the atlas if the atlas holds its coverage. Returns false if the path must be filled or
// stroked as a path instead.
bool drawAtlasPath(Walk& w, const PrDrawCommand& c, const Ink& ink) {
    const PrDrawTransform& x = w.transforms.last();
    if (c.kind != PrDrawCommandKind_Path || !c.antialias)
        return false;
    Ink drawn = ink;
    PrDrawStroke stroke{};
    if (c.stroke != nullptr && !drawnStroke(w, c.stroke, c.antialias, &stroke, &drawn))
        return true;
    uint64_t rasterized = prDrawAtlasInfo(w.f->atlas).rasterizations;
    PrDrawAtlasEntry e{};
    if (!prDrawAtlasPath(w.f->atlas, c.path, c.rule, c.stroke != nullptr ? &stroke : nullptr, x.affine, &e))
        return false;
    w.b->pathFills += uint32_t(prDrawAtlasInfo(w.f->atlas).rasterizations - rasterized);
    drawAtlasEntry(w, e, drawn);
    return true;
}

// Draws a glyph run. Each glyph draws on its own. A filled color glyph plays its list. Other glyphs come from the
// atlas, or are filled or stroked as paths.
void drawGlyphs(Walk& w, const PrDrawCommand& c) {
    const PrFontGlyphRun& run = *c.glyphs;
    StrokeMapping mapping{};
    bool mapped = mapsStroke(c);
    if (mapped && !mapStroke(w, c, &mapping))
        return;
    FragmentPaint r{};
    float color[4];
    if (!makePaint(w, c.brush, c.color, prDrawFillBounds(&c), &r, color, mapped ? &mapping.unitPixels : nullptr))
        return;
    if (mapped) {
        r.map = mapping.map;
        r.brush = prMatrix3FromAffine2(mapping.unitToBrush);
    }
    Ink ink = inkOf(w, r, color);
    PrColor foreground = c.color;
    foreground.a = 1;
    for (uint32_t i = 0; i < run.glyphCount; ++i) {
        if (c.stroke == nullptr) {
            float pixelsPerEm = prDrawGlyphPixelsPerEm(&run, i, w.transforms.last().affine);
            PrDrawList* drawn =
                prDrawGlyphCacheList(w.f->lists, run.font, run.glyphs[i].id, pixelsPerEm, &run.palette, foreground);
            if (drawn != nullptr) {
                PrDrawGroup group{};
                PrDrawCommand commands[kPrDrawGlyphMaxCommands];
                runCommands(w, commands, prDrawGlyphListCommands(&c, i, drawn, &group, commands));
                continue;
            }
            if (drawAtlasGlyph(w, c, i, ink))
                continue;
        }
        const PrDrawTransform& x = w.transforms.last();
        prPathClear(w.f->shape);
        if (!prDrawGlyphPath(&run, i, &x.affine, w.f->shape))
            continue;
        if (c.stroke != nullptr)
            strokePath(w, w.f->shape, c.stroke, c.antialias, ink);
        else
            fillPath(w, w.f->shape, PrPathFillRule_NonZero, c.antialias, ink);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers

// Starts a pass into the layer. If the last pass did nothing, it is reused instead of beginning a pass on the device
// for nothing. Such a pass draws nothing, filters nothing, has the host draw nothing, and clears nothing or clears the
// same layer.
void beginPass(Walk& w, uint32_t layer, bool clear) {
    if (w.b->passes.count != 0) {
        FragmentPass& last = w.b->passes.last();
        if (last.count == 0 && last.filter == 0 && !last.beneath && !last.mipmaps && (!last.clear || last.layer == layer)) {
            last.clear = last.clear || clear;
            last.layer = layer;
            last.first = w.b->instances.count;
            w.pass = w.b->passes.count - 1;
            return;
        }
    }
    w.pass = w.b->passes.count;
    w.b->passes.push(FragmentPass{.layer = layer, .clear = clear, .first = w.b->instances.count});
}

void endPass(Walk& w) {
    FragmentPass& pass = w.b->passes.data[w.pass];
    pass.count = w.b->instances.count - pass.first;
}

// A clip that lets through the whole box, and nothing outside it.
Clip clipOfBox(const Walk& w, PrIRect box) {
    Clip clip{};
    clip.box = box;
    clip.rect = PrRect{float(box.left), float(box.top), float(box.right), float(box.bottom)};
    clip.first = w.clipShapes.count;
    clip.whole = true;
    return clip;
}

// Starts drawing into a new layer over the box, which is not empty, and returns its texture. Drawing into the layer is
// clipped by the current clips if `clipped`, and otherwise only by the box.
uint32_t enterLayer(Walk& w, PrIRect box, bool clipped, bool precise) {
    endPass(w);
    uint32_t texture = ++w.f->nextTexture;
    uint32_t layer = w.b->layers.count + 1;
    w.f->layerIndex.insert(texture, w.b->layers.count);
    w.b->layers.push(FragmentLayer{
        .texture = texture,
        .origin = {box.left, box.top},
        .extent = {uint32_t(prIRectWidth(box)), uint32_t(prIRectHeight(box))},
        .precise = precise || w.extended,
    });
    w.layers.push(layer);
    w.knockouts.push(false);
    w.tracks.push(false);
    w.drawn.push(PrIRect{0, 0, 0, 0});
    beginPass(w, layer, true);
    Clip clip = clipOfBox(w, box);
    if (clipped) {
        clip = clipOf(w);
        clip.box = prIRectIntersect(clip.box, box);
    }
    w.clips.push(clip);
    w.opacities.push(1);
    return texture;
}

// Returns to drawing into the surface that the current layer's drawing interrupted. Returns the bounds of what was
// drawn into the layer if the walk tracked them, and otherwise the layer's box.
PrIRect leaveLayer(Walk& w) {
    endPass(w);
    const FragmentLayer& entered = w.b->layers.data[w.layers.last() - 1];
    PrIRect drawn = w.tracks.last() ? w.drawn.last() : prIRectAt(entered.origin, entered.extent);
    w.tracks.pop();
    w.drawn.pop();
    w.layers.pop();
    w.knockouts.pop();
    w.clips.pop();
    w.opacities.pop();
    beginPass(w, w.layers.last(), false);
    return drawn;
}

// Returns a paint record that paints the color alone with the walk's operator and blend mode. Writes the color, which
// is white at `opacity`, to `color`.
FragmentPaint layerPaint(const Walk& w, float opacity, float color[4]) {
    for (uint32_t i = 0; i < 4; ++i)
        color[i] = opacity;
    return plainPaint(w);
}

// Draws the layer over the box through the record, whose compositing is set, times the color.
void compositeLayer(Walk& w, uint32_t texture, PrIRect box, FragmentPaint& r, const float color[4]) {
    r.kind = FragmentPaintKind_Layer;
    r.texture = tableOf(w, texture);
    drawSolid(w, box, inkOf(w, r, color));
}

const FragmentLayer& layerOf(const Walk& w, uint32_t texture) {
    uint32_t i = 0;
    while (w.b->layers.data[i].texture != texture)
        ++i;
    return w.b->layers.data[i];
}

FragmentLayer& layerRecord(Walk& w, uint32_t texture) {
    return const_cast<FragmentLayer&>(layerOf(w, texture));
}

// Makes the layer's texture exactly its extent with every mip level, and fills the levels after the first from what has
// been drawn into the layer.
void mipmapLayer(Walk& w, uint32_t texture) {
    FragmentLayer& layer = layerRecord(w, texture);
    layer.exact = true;
    layer.mipmapped = true;
    uint32_t index = uint32_t(&layer - w.b->layers.data) + 1;
    endPass(w);
    w.b->passes.push(FragmentPass{.layer = index, .mipmaps = true, .first = w.b->instances.count});
    beginPass(w, w.layers.last(), false);
}

// Returns true if the host's texture holds what lies beneath the target. The frame's beneath layer names that texture.
bool beneathTextured(const Walk& w) {
    return w.beneath != nullptr && w.beneath->kind == PrDrawBeneathKind_Texture;
}

// Returns true if the host redraws what lies beneath the target into the passes that need it.
bool beneathDrawn(const Walk& w) {
    return w.beneath != nullptr && w.beneath->kind == PrDrawBeneathKind_Draw;
}

// Returns true if a color lies beneath the target. The frame draws that color itself.
bool beneathColored(const Walk& w) {
    return w.beneath != nullptr && w.beneath->kind == PrDrawBeneathKind_Color && prFloat16ToFloat(w.beneath->color.a) > 0;
}

// Draws the color that lies beneath the target over the box of the current layer.
void drawBeneathColor(Walk& w, PrIRect box) {
    FragmentPaint r{};
    float color[4];
    PrRect rect{float(box.left), float(box.top), float(box.right), float(box.bottom)};
    if (makePaint(w, nullptr, w.beneath->color, rect, &r, color))
        drawSolid(w, box, inkOf(w, r, color));
}

// Returns the texture of the layer that the host's texture fills, which is created on first use. The layer covers the
// target, and no pass draws into it.
uint32_t beneathTexture(Walk& w) {
    if (w.b->beneath == 0) {
        uint32_t texture = ++w.f->nextTexture;
        w.b->beneath = w.b->layers.count + 1;
        w.f->layerIndex.insert(texture, w.b->layers.count);
        w.b->layers.push(
            FragmentLayer{.texture = texture, .extent = {uint32_t(w.target.right), uint32_t(w.target.bottom)}});
    }
    return w.b->layers.data[w.b->beneath - 1].texture;
}

// Creates a layer over the box, into which the host draws what lies beneath the target, and returns its texture. The
// host draws in a pass of its own that draws nothing else.
uint32_t drawnBeneath(Walk& w, PrIRect box) {
    uint32_t texture = enterLayer(w, box, false);
    w.b->passes.data[w.pass].beneath = true;
    leaveLayer(w);
    return texture;
}

// Returns a texture that reads as `top` over `under`, or over the premultiplied color if `under` is zero.
uint32_t stackedOf(Walk& w, uint32_t top, uint32_t under, PrColor16 color) {
    uint32_t texture = ++w.f->nextTexture;
    w.stacked.push(Stacked{texture, top, under, color});
    return texture;
}

uint32_t projectedBeneath(Walk& w, uint32_t layer);
void placeLayer(Walk& w, FragmentPaint& r, const FragmentLayer& layer, const PrMatrix3& toSpace);

// Writes the textures that a backdrop reads beneath drawing into the layer, bottom first, and returns their number.
// They are in the layer's pixels, over `read`. The layer is read over what lies beneath the target if it is the list's
// own layer, and over what lies beneath a projected group's plane if it holds that plane. Writes whether the color
// beneath the target lies under them.
uint32_t texturesUnder(Walk& w, uint32_t layer, PrIRect read, uint32_t out[2], bool* colored) {
    *colored = false;
    uint32_t top = w.b->layers.data[layer - 1].texture;
    if (layer != w.root) {
        uint32_t below = projectedBeneath(w, layer);
        uint32_t count = 0;
        if (below != 0)
            out[count++] = below;
        out[count++] = top;
        return count;
    }
    uint32_t count = 0;
    if (beneathTextured(w))
        out[count++] = beneathTexture(w);
    else if (beneathDrawn(w) && !prIRectIsEmpty(read))
        out[count++] = drawnBeneath(w, read);
    *colored = beneathColored(w);
    if (!prIRectIsEmpty(prIRectIntersect(w.drawn.data[1], read)))
        out[count++] = top;
    return count;
}

// Returns a new layer over `box` that holds what a backdrop reads beneath drawing into the layer `beneath`, resampled
// through `toDevice`, which maps the new layer's pixels to the pixels of `beneath`.
uint32_t resampledUnder(Walk& w, uint32_t beneath, const PrMatrix3& toDevice, PrIRect box) {
    PrIRect read = mappedBox(toDevice, box, w.target);
    uint32_t layers[2] = {};
    bool colored = false;
    uint32_t count = texturesUnder(w, beneath, read, layers, &colored);
    uint32_t under = enterLayer(w, box, false);
    if (colored)
        drawBeneathColor(w, box);
    for (uint32_t i = 0; i < count; ++i) {
        float color[4];
        FragmentPaint r = layerPaint(w, 1, color);
        placeLayer(w, r, layerOf(w, layers[i]), toDevice);
        drawSolid(w, box, inkOf(w, r, color));
    }
    leaveLayer(w);
    return under;
}

// Returns the texture of what lies beneath the plane of the projected group whose layer is `layer`, resampled into that
// layer, or 0 if `layer` is not the layer of a projected group that is not a backdrop root. It is built the first time
// a backdrop reads it, which is before anything draws over the group's parent again.
uint32_t projectedBeneath(Walk& w, uint32_t layer) {
    for (uint32_t i = w.groups.count; i-- > 0;) {
        if (w.groups.data[i].layer != layer)
            continue;
        if (!w.groups.data[i].projected || prDrawGroupIsBackdropRoot(w.groups.data[i].group))
            return 0;
        if (w.groups.data[i].beneath == 0)
            return 0;
        if (w.groups.data[i].under == 0) {
            const FragmentLayer drawn = w.b->layers.data[layer - 1];
            GroupFrame frame = w.groups.data[i];
            uint32_t under = resampledUnder(w, frame.beneath, frame.toDevice, prIRectAt(drawn.origin, drawn.extent));
            w.groups.data[i].under = uint32_t(&layerOf(w, under) - w.b->layers.data) + 1;
        }
        return w.b->layers.data[w.groups.data[i].under - 1].texture;
    }
    return 0;
}

// Returns the texture of what lies beneath a group, over the box. This is the layer that the group draws over. If that
// is the list's own layer, the texture is that layer read over what lies beneath the target, and if it holds a
// projected group's plane, that layer read over what lies beneath the plane. Only a host redraw takes a pass. What lies
// beneath the target is read alone only if the list drew nothing in the box.
uint32_t underOf(Walk& w, const GroupFrame& frame, PrIRect box) {
    if (frame.under == 0)
        return 0;
    uint32_t under = w.b->layers.data[frame.under - 1].texture;
    if (frame.under != w.root) {
        uint32_t below = projectedBeneath(w, frame.under);
        return below != 0 ? stackedOf(w, under, below, PrColor16{}) : under;
    }
    bool drew = !prIRectIsEmpty(prIRectIntersect(w.drawn.data[1], box));
    uint32_t beneath = beneathTextured(w) ? beneathTexture(w) : beneathDrawn(w) ? drawnBeneath(w, box) : 0;
    if (beneath != 0)
        return drew ? stackedOf(w, under, beneath, PrColor16{}) : beneath;
    if (beneathColored(w)) {
        PrColor c = w.beneath->color;
        float a = prF32Clamp(c.a, 0, 1);
        return stackedOf(w, under, 0, recordColor(w, PrColor{c.r * a, c.g * a, c.b * a, a}));
    }
    return under;
}

// Makes the record paint a layer at the position that another space gives it, sampled between its pixels and
// transparent outside it. `toSpace` maps the pixels drawn into to the space of the layer's pixels. The texels beyond
// the layer's extent are transparent, which is the gutter of a tile that extends by None.
void placeLayer(Walk& w, FragmentPaint& r, const FragmentLayer& layer, const PrMatrix3& toSpace) {
    r.brush = toSpace;
    r.kind = FragmentPaintKind_Image;
    r.sampling = PrDrawSampling_Linear;
    r.read = FragmentImageRead_Sampler;
    r.texels = PrRect{0, 0, float(layer.extent.x), float(layer.extent.y)};
    r.tile = prRectFromIRect(prIRectAt(layer.origin, layer.extent));
    r.tileX = PrDrawExtend_None;
    r.tileY = PrDrawExtend_None;
    r.texture = tableOf(w, layer.texture);
}

// ---------------------------------------------------------------------------------------------------------------------
// Filters

// A filter graph being evaluated into layers over a group's box.
struct FilterRun {
    PrIRect box;                // the area that every step's layer covers
    PrIRect region;             // the area where the graph produces output
    PrDrawTransform transform;  // the group's space to target pixels
    float scaleX;               // target pixels per unit of the group's space along each axis
    float scaleY;
    uint32_t source;            // the layers of the inputs
    uint32_t sourceAlpha;       // created on first use
    uint32_t backdrop;          // zero if there is none
    uint32_t empty;             // a transparent layer, created on first use
    RawArray<uint32_t> results;
    RawArray<PrIRect> regions;  // the region of each result
    PrRect graph;               // the area where the graph produces output, in the group's space
};

FragmentFilter stepOf(FragmentFilterOp op, uint32_t flags = 0) {
    FragmentFilter step{};
    step.op = op;
    step.flags = flags;
    return step;
}

// Draws the step into a new precise layer over the run's box, over `region` within it, and returns the layer's texture.
uint32_t filterStep(Walk& w, const FilterRun& run, PrIRect region, FragmentFilter step, uint32_t a,
        uint32_t b = 0, uint32_t c = 0) {
    uint32_t texture = enterLayer(w, run.box, false, true);
    region = prIRectIntersect(region, run.box);
    if (!prIRectIsEmpty(region)) {
        step.inputs[0] = tableOf(w, a);
        step.inputs[1] = tableOf(w, b);
        step.inputs[2] = tableOf(w, c);
        w.b->passes.data[w.pass].filter = w.b->filters.count + 1;
        w.b->filters.push(step);
        FragmentInstance i = boxInstance(region, FragmentCoverage_Solid);
        i.color = PrColor{1, 1, 1, 1};
        w.b->instances.push(i);
    }
    leaveLayer(w);
    return texture;
}

// Appends values to the frame's filter data and records their position in the step.
void addData(Walk& w, FragmentFilter& step, const float* values, uint32_t count) {
    step.data = w.b->filterData.count;
    step.dataCount = count;
    w.b->filterData.append(values, count);
}

// Returns the flags of a step that reads its inputs and writes its result in the node's space.
uint32_t spaceFlags(const PrDrawFilterNode& node) {
    if (node.space != PrColorSpace_SrgbLinear)
        return 0;
    return FragmentFilterFlag_LinearInputs | FragmentFilterFlag_LinearResult;
}

// Writes the rectangle beyond which an area step reads by its edge mode: the region of its input.
void edgeRect(PrIRect edges, float* out) {
    out[0] = float(edges.left);
    out[1] = float(edges.top);
    out[2] = float(edges.right);
    out[3] = float(edges.bottom);
}

// Returns the region of an input: the graph's for the group's inputs, and the region of the node that made a result.
PrIRect inputRegion(const FilterRun& run, int32_t which) {
    return which >= 0 && uint32_t(which) < run.regions.count ? run.regions.data[which] : run.region;
}

// Blurs the layer by a Gaussian with sigmas in target pixels, as two steps, reading beyond `edges` by the edge mode.
// `first` holds the flags for the first step's reading, and `last` the flags for the second step's writing.
uint32_t blurSteps(Walk& w, const FilterRun& run, PrIRect region, uint32_t in, float sigmaX, float sigmaY,
        PrImageEdgeMode edge, PrIRect edges, uint32_t first, uint32_t last) {
    FragmentFilter x = stepOf(FragmentFilterOp_BlurX, first);
    x.values[0] = prF32Max(sigmaX, 0);
    x.values[1] = float(edge);
    edgeRect(edges, x.values + 2);
    uint32_t across = filterStep(w, run, run.box, x, in);
    FragmentFilter y = stepOf(FragmentFilterOp_BlurY, last);
    y.values[0] = prF32Max(sigmaY, 0);
    y.values[1] = float(edge);
    edgeRect(edges, y.values + 2);
    return filterStep(w, run, region, y, across);
}

// Takes each channel's largest or smallest value within radii in target pixels, as two steps.
uint32_t morphologySteps(Walk& w, const FilterRun& run, PrIRect region, uint32_t in, float radiusX, float radiusY,
        PrDrawMorphologyOp op) {
    FragmentFilter x = stepOf(FragmentFilterOp_MorphologyX);
    x.values[0] = prF32Min(prF32Floor(prF32Max(radiusX, 0)), 256);
    x.values[1] = float(op);
    uint32_t across = filterStep(w, run, run.box, x, in);
    FragmentFilter y = stepOf(FragmentFilterOp_MorphologyY);
    y.values[0] = prF32Min(prF32Floor(prF32Max(radiusY, 0)), 256);
    y.values[1] = float(op);
    return filterStep(w, run, region, y, across);
}

// Writes the step's color values, which are a straight color's channels, in the range that the frame holds.
void colorValues(const Walk& w, PrColor c, float* out) {
    out[0] = w.extended ? c.r : prF32Clamp(c.r, 0, 1);
    out[1] = w.extended ? c.g : prF32Clamp(c.g, 0, 1);
    out[2] = w.extended ? c.b : prF32Clamp(c.b, 0, 1);
    out[3] = prF32Clamp(c.a, 0, 1);
}

// Returns an offset in the group's space, converted to target pixels.
PrVec2 deviceOffset(const FilterRun& run, PrVec2 offset) {
    const PrAffine2& t = run.transform.affine;
    return PrVec2{t.a * offset.x + t.c * offset.y, t.b * offset.x + t.d * offset.y};
}

uint32_t filterInput(Walk& w, FilterRun& run, int32_t which) {
    switch (which) {
        case PrDrawFilterInput_Source:
            return run.source;
        case PrDrawFilterInput_SourceAlpha:
            if (run.sourceAlpha == 0)
                run.sourceAlpha = filterStep(w, run, run.box, stepOf(FragmentFilterOp_Alpha), run.source);
            return run.sourceAlpha;
        case PrDrawFilterInput_Backdrop:
            if (run.backdrop != 0)
                return run.backdrop;
            [[fallthrough]];
        default:
            if (which >= 0 && uint32_t(which) < run.results.count)
                return run.results.data[which];
            if (run.empty == 0)
                run.empty = filterStep(w, run, PrIRect{0, 0, 0, 0}, stepOf(FragmentFilterOp_Flood), 0);
            return run.empty;
    }
}

// Draws an image node's source into a layer over its rect, within the region.
uint32_t imageNode(Walk& w, const FilterRun& run, const PrDrawFilterNode& node, PrIRect region) {
    uint32_t texture = enterLayer(w, run.box, false, true);
    w.transforms.push(run.transform);
    PrDrawCommand clip{};
    clip.kind = PrDrawCommandKind_PushClipTargetRect;
    clip.rect = PrRect{float(region.left), float(region.top), float(region.right), float(region.bottom)};
    pushClip(w, clip);
    PrDrawBrush brush = prDrawBrushImage(node.image.source, node.image.rect, node.image.sampling);
    PrDrawCommand image{};
    image.kind = PrDrawCommandKind_Rect;
    image.antialias = true;
    image.color = PrColor{1, 1, 1, 1};
    image.brush = &brush;
    image.rect = node.image.rect;
    drawShape(w, image);
    popClip(w);
    w.transforms.pop();
    leaveLayer(w);
    return texture;
}

// Evaluates one node into a layer, as steps.
uint32_t filterNode(Walk& w, FilterRun& run, const PrDrawFilterNode& node, PrIRect region) {
    uint32_t flags = spaceFlags(node);
    bool linear = node.space == PrColorSpace_SrgbLinear;
    uint32_t firstIn = linear ? uint32_t(FragmentFilterFlag_LinearInputs) : 0;
    uint32_t lastOut = linear ? uint32_t(FragmentFilterFlag_LinearResult) : 0;
    auto in = [&](uint32_t i) { return filterInput(w, run, node.inputs[i]); };
    float sx = run.scaleX;
    float sy = run.scaleY;
    switch (node.kind) {
        case PrDrawFilterKind_Offset: {
            FragmentFilter step = stepOf(FragmentFilterOp_Offset, flags);
            PrVec2 d = deviceOffset(run, node.offset);
            step.values[0] = d.x;
            step.values[1] = d.y;
            return filterStep(w, run, region, step, in(0));
        }
        case PrDrawFilterKind_Flood: {
            // The color in the node's space, which the result is converted back from.
            FragmentFilter step = stepOf(FragmentFilterOp_Flood, lastOut);
            float straight[4];
            colorValues(w, node.flood, straight);
            float a = straight[3];
            for (uint32_t c = 0; c < 3; ++c)
                step.values[c] = (linear ? prColorTransferDecode(PrColorTransfer_Srgb, straight[c]) : straight[c]) * a;
            step.values[3] = a;
            return filterStep(w, run, region, step, 0);
        }
        case PrDrawFilterKind_ColorMatrix: {
            FragmentFilter step = stepOf(FragmentFilterOp_ColorMatrix, flags);
            addData(w, step, &node.matrix.m[0][0], 20);
            return filterStep(w, run, region, step, in(0));
        }
        case PrDrawFilterKind_ComponentTransfer: {
            FragmentFilter step = stepOf(FragmentFilterOp_Transfer, flags);
            RawArray<float> data;
            for (const PrDrawTransfer& t : node.transfer) {
                bool tabled = t.kind == PrDrawTransferKind_Table || t.kind == PrDrawTransferKind_Discrete;
                uint32_t count = tabled ? t.valueCount : 0;
                float head[7] = {
                    float(t.kind), t.slope,  t.intercept, t.amplitude,
                    t.exponent,    t.offset, float(count),
                };
                data.append(head, 7);
                if (count != 0)
                    data.append(t.values, count);
            }
            addData(w, step, data.data, data.count);
            data.reset();
            return filterStep(w, run, region, step, in(0));
        }
        case PrDrawFilterKind_Composite:
        case PrDrawFilterKind_Blend: {
            FragmentFilter step = stepOf(FragmentFilterOp_Composite, flags);
            PrDrawComposite composite;
            composite = node.kind == PrDrawFilterKind_Composite ? node.composite : PrDrawComposite_SourceOver;
            step.values[0] = float(composite);
            step.values[1] = float(node.kind == PrDrawFilterKind_Blend ? node.blend : PrDrawBlendMode_Normal);
            return filterStep(w, run, region, step, in(0), in(1));
        }
        case PrDrawFilterKind_Arithmetic: {
            FragmentFilter step = stepOf(FragmentFilterOp_Arithmetic, flags);
            prMemCopy(step.values, node.k, sizeof(node.k));
            return filterStep(w, run, region, step, in(0), in(1));
        }
        case PrDrawFilterKind_Merge: {
            // Composites each input over the result of the inputs before it, in the node's space throughout.
            uint32_t made = filterInput(w, run, -1000);
            for (uint32_t k = 0; k < node.merge.inputCount; ++k) {
                uint32_t stepFlags = firstIn | (k + 1 == node.merge.inputCount ? lastOut : 0);
                FragmentFilter step = stepOf(FragmentFilterOp_Over, stepFlags);
                made = filterStep(w, run, k + 1 == node.merge.inputCount ? region : run.box, step,
                    filterInput(w, run, node.merge.inputs[k]), made);
            }
            if (node.merge.inputCount == 0)
                return filterStep(w, run, region, stepOf(FragmentFilterOp_Flood), 0);
            return made;
        }
        case PrDrawFilterKind_Blur:
            return blurSteps(w, run, region, in(0), node.blur.sigma.x * sx, node.blur.sigma.y * sy, node.blur.edge,
                inputRegion(run, node.inputs[0]), firstIn, lastOut);
        case PrDrawFilterKind_ProgressiveBlur: {
            // The brush's alpha selects a sigma. Mixing between levels of blur that double in sigma reaches that sigma.
            uint32_t source = in(0);
            uint32_t alpha = enterLayer(w, run.box, false, true);
            w.transforms.push(run.transform);
            FragmentPaint r{};
            float color[4];
            PrRect area = prRectIsEmpty(node.region) ? PrRect{} : node.region;
            if (makePaint(w, &node.progressive.brush, PrColor{1, 1, 1, 1}, area, &r, color))
                drawSolid(w, run.box, inkOf(w, r, color));
            w.transforms.pop();
            leaveLayer(w);
            uint32_t made = source;
            float low = 0;
            for (uint32_t i = 1; i < kProgressiveLevels; ++i) {
                float high = 1 / float(1u << (kProgressiveLevels - 1 - i));
                float sigma = node.progressive.sigma * high;
                uint32_t level = blurSteps(w, run, run.box, source, sigma * sx, sigma * sy, PrImageEdgeMode_None,
                    run.box, firstIn, lastOut);
                FragmentFilter step = stepOf(FragmentFilterOp_Mix, flags);
                step.values[0] = low;
                step.values[1] = high;
                made = filterStep(w, run, i + 1 == kProgressiveLevels ? region : run.box, step, made, level, alpha);
                low = high;
            }
            return made;
        }
        case PrDrawFilterKind_Morphology: {
            uint32_t made = morphologySteps(w, run, run.box, in(0), node.morphology.radius.x * sx,
                node.morphology.radius.y * sy, node.morphology.op);
            return filterStep(w, run, region, stepOf(FragmentFilterOp_Offset), made);
        }
        case PrDrawFilterKind_Convolution: {
            const PrDrawFilterConvolution& k = node.convolution;
            FragmentFilter step = stepOf(FragmentFilterOp_Convolution, flags);
            float values[12] = {
                float(k.extent.x), float(k.extent.y),     k.divisor,
                k.bias,           float(k.target.x),  float(k.target.y),
                float(k.edge),    k.preserveAlpha ? 1.0f : 0.0f,
            };
            edgeRect(inputRegion(run, node.inputs[0]), values + 8);
            prMemCopy(step.values, values, sizeof(values));
            addData(w, step, k.kernel, k.extent.x * k.extent.y);
            return filterStep(w, run, region, step, in(0));
        }
        case PrDrawFilterKind_Turbulence: {
            FragmentFilter step = stepOf(FragmentFilterOp_Turbulence, lastOut);
            PrAffine2 fromDevice{};
            if (!prAffine2Invert(run.transform.affine, &fromDevice))
                return filterInput(w, run, -1000);
            // A stitched tile is the node's region in the group's space, or the graph's.
            PrRect tile = prRectIsEmpty(node.region) ? run.graph : node.region;
            const PrDrawFilterTurbulence& t = node.turbulence;
            float head[15] = {
                fromDevice.a,
                fromDevice.b,
                fromDevice.c,
                fromDevice.d,
                fromDevice.e,
                fromDevice.f,
                t.baseFrequency.x,
                t.baseFrequency.y,
                float(t.octaves),
                float(t.kind),
                t.stitch ? 1.0f : 0.0f,
                tile.left,
                tile.top,
                tile.right,
                tile.bottom,
            };
            PrImageNoise* noise = alloc<PrImageNoise>();
            prImageNoise(t.seed, noise);
            RawArray<float> data;
            data.append(head, 15);
            for (int32_t v : noise->lattice)
                data.push(float(v));
            data.append(&noise->gradients[0][0][0], uint32_t(sizeof(noise->gradients) / sizeof(float)));
            prMemoryFree(noise);
            addData(w, step, data.data, data.count);
            data.reset();
            return filterStep(w, run, region, step, 0);
        }
        case PrDrawFilterKind_Image:
            return imageNode(w, run, node, region);
        case PrDrawFilterKind_Tile: {
            FragmentFilter step = stepOf(FragmentFilterOp_Tile);
            PrIRect tile = prIRectIntersect(prIRectRoundOut(prAffine2MapRect(run.transform.affine, node.tile)), run.box);
            float values[4] = {float(tile.left), float(tile.top), float(tile.right), float(tile.bottom)};
            prMemCopy(step.values, values, sizeof(values));
            return filterStep(w, run, region, step, in(0));
        }
        case PrDrawFilterKind_DropShadow: {
            uint32_t source = in(0);
            uint32_t alpha = filterStep(w, run, run.box, stepOf(FragmentFilterOp_Alpha), source);
            uint32_t blurred = blurSteps(w, run, run.box, alpha, node.shadow.sigma.x * sx, node.shadow.sigma.y * sy,
                PrImageEdgeMode_None, run.box, 0, 0);
            FragmentFilter move = stepOf(FragmentFilterOp_Offset);
            PrVec2 d = deviceOffset(run, node.shadow.offset);
            move.values[0] = d.x;
            move.values[1] = d.y;
            uint32_t moved = filterStep(w, run, run.box, move, blurred);
            FragmentFilter color = stepOf(FragmentFilterOp_Colorize);
            colorValues(w, node.shadow.color, color.values);
            uint32_t shadow = filterStep(w, run, run.box, color, moved);
            return filterStep(w, run, region, stepOf(FragmentFilterOp_Over, firstIn | lastOut), source, shadow);
        }
        case PrDrawFilterKind_InnerShadow: {
            uint32_t source = in(0);
            uint32_t empty = filterStep(w, run, run.box, stepOf(FragmentFilterOp_InverseAlpha), source);
            FragmentFilter move = stepOf(FragmentFilterOp_Offset);
            PrVec2 d = deviceOffset(run, node.shadow.offset);
            move.values[0] = d.x;
            move.values[1] = d.y;
            uint32_t moved = filterStep(w, run, run.box, move, empty);
            uint32_t blurred = blurSteps(w, run, run.box, moved, node.shadow.sigma.x * sx, node.shadow.sigma.y * sy,
                PrImageEdgeMode_Duplicate, run.box, 0, 0);
            FragmentFilter color = stepOf(FragmentFilterOp_Colorize);
            colorValues(w, node.shadow.color, color.values);
            uint32_t shadow = filterStep(w, run, run.box, color, blurred);
            return filterStep(w, run, region, stepOf(FragmentFilterOp_Atop,
                (linear ? uint32_t(FragmentFilterFlag_LinearInputs) : 0) | lastOut), shadow, source);
        }
        case PrDrawFilterKind_OuterGlow:
        case PrDrawFilterKind_InnerGlow: {
            bool outer = node.kind == PrDrawFilterKind_OuterGlow;
            uint32_t source = in(0);
            uint32_t alpha = filterStep(
                w, run, run.box, stepOf(outer ? FragmentFilterOp_Alpha : FragmentFilterOp_InverseAlpha), source);
            uint32_t grown = morphologySteps(w, run, run.box, alpha, node.glow.spread * sx, node.glow.spread * sy, true);
            uint32_t blurred = blurSteps(w, run, run.box, grown, node.glow.sigma * sx, node.glow.sigma * sy,
                outer ? PrImageEdgeMode_None : PrImageEdgeMode_Duplicate, run.box, 0, 0);
            FragmentFilter color = stepOf(FragmentFilterOp_Colorize);
            colorValues(w, node.glow.color, color.values);
            uint32_t glow = filterStep(w, run, run.box, color, blurred);
            if (outer)
                return filterStep(w, run, region, stepOf(FragmentFilterOp_Over, firstIn | lastOut), source, glow);
            return filterStep(w, run, region, stepOf(FragmentFilterOp_Atop,
                (linear ? uint32_t(FragmentFilterFlag_LinearInputs) : 0) | lastOut), glow, source);
        }
        case PrDrawFilterKind_Feather: {
            uint32_t source = in(0);
            float half = node.feather / 2;
            uint32_t alpha = filterStep(w, run, run.box, stepOf(FragmentFilterOp_Alpha), source);
            uint32_t shrunk = morphologySteps(w, run, run.box, alpha, half * sx, half * sy, false);
            uint32_t soft = blurSteps(w, run, run.box, shrunk, half * sx, half * sy, PrImageEdgeMode_None, run.box, 0, 0);
            return filterStep(w, run, region, stepOf(FragmentFilterOp_Mask, firstIn | lastOut), source, soft);
        }
        case PrDrawFilterKind_DisplacementMap: {
            // The map is read in the node's space, and the first input's pixels move unconverted.
            uint32_t flagsIn = linear ? uint32_t(FragmentFilterFlag_LinearInputs) : 0;
            FragmentFilter step = stepOf(FragmentFilterOp_Displacement, flagsIn | FragmentFilterFlag_RawFirst);
            step.values[0] = node.displacement.scale * sx;
            step.values[1] = node.displacement.scale * sy;
            step.values[2] = float(node.displacement.x);
            step.values[3] = float(node.displacement.y);
            return filterStep(w, run, region, step, in(0), in(1));
        }
        case PrDrawFilterKind_Lighting: {
            const PrDrawFilterLighting& l = node.lighting;
            FragmentFilter step = stepOf(FragmentFilterOp_Lighting, flags);
            float scale = prF32Sqrt(sx * sy);
            PrVec2 at = prAffine2MapPoint(run.transform.affine, PrVec2{l.light.position.x, l.light.position.y});
            PrVec2 target = prAffine2MapPoint(run.transform.affine, PrVec2{l.light.pointsAt.x, l.light.pointsAt.y});
            float values[19] = {
                float(l.kind),
                l.surfaceScale,
                l.constant,
                l.specularExponent,
                prFloat16ToFloat(l.color.r),
                prFloat16ToFloat(l.color.g),
                prFloat16ToFloat(l.color.b),
                float(l.light.kind),
                l.light.azimuth,
                l.light.elevation,
                at.x,
                at.y,
                l.light.position.z * scale,
                target.x,
                target.y,
                l.light.pointsAt.z * scale,
                l.light.spotExponent,
                l.light.coneAngle > 0 ? prF32Cos(l.light.coneAngle) : -1,
                scale,
            };
            addData(w, step, values, 19);
            return filterStep(w, run, region, step, in(0));
        }
        default:
            return filterInput(w, run, -1000);
    }
}

// Evaluates the filter over the layer that `source` names, and returns the layer of its result. `transform` maps the
// group's space to target pixels. `bounds` is the group's bounds, used for a filter without a region.
uint32_t runFilter(Walk& w, const PrDrawFilter* filter, uint32_t source, uint32_t backdrop, PrIRect box,
        const PrDrawTransform& transform, PrRect bounds) {
    FilterRun run{};
    run.box = box;
    run.transform = transform;
    const PrAffine2& toDevice = transform.affine;
    run.scaleX = prF32Sqrt(toDevice.a * toDevice.a + toDevice.b * toDevice.b);
    run.scaleY = prF32Sqrt(toDevice.c * toDevice.c + toDevice.d * toDevice.d);
    run.source = source;
    run.backdrop = backdrop;
    PrRect region = prDrawFilterRegion(filter);
    if (prRectIsEmpty(region))
        region = bounds;
    if (prRectIsEmpty(region))
        run.region = box;
    else
        run.region = prIRectIntersect(box, prIRectRoundOut(prAffine2MapRect(run.transform.affine, region)));
    run.graph = region;
    PrAffine2 fromDevice{};
    if (prRectIsEmpty(region) && prAffine2Invert(run.transform.affine, &fromDevice))
        run.graph = prAffine2MapRect(fromDevice, prRectFromIRect(box));
    const PrDrawFilterNode* nodes = prDrawFilterNodes(filter);
    for (uint32_t i = 0; i < prDrawFilterNodeCount(filter); ++i) {
        PrIRect nodeRegion = run.region;
        if (!prRectIsEmpty(nodes[i].region))
            nodeRegion = prIRectIntersect(run.region,
                prIRectRoundOut(prAffine2MapRect(run.transform.affine, nodes[i].region)));
        run.results.push(filterNode(w, run, nodes[i], nodeRegion));
        run.regions.push(nodeRegion);
    }
    uint32_t result = run.results.count != 0 ? run.results.last() : source;
    run.results.reset();
    run.regions.reset();
    return result;
}

// Returns how far, in target pixels, the filter may move what it reads, under the transform.
int32_t filterReach(const PrDrawFilter* filter, const PrDrawTransform& transform) {
    PrRect outset = prDrawFilterOutset(filter);
    float reach = prF32Max(prF32Max(-outset.left, -outset.top), prF32Max(outset.right, outset.bottom));
    float scale = prAffine2MaxScale(transform.affine);
    return int32_t(prF32Ceil(prF32Min(reach * scale, 1 << 28))) + 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Groups

// Returns the device box that a group's contents may cover: the clip's box, within the group's bounds if it has them.
PrIRect groupBox(const Walk& w, const PrDrawGroup& g, PrIRect visible) {
    if (prRectIsEmpty(g.bounds))
        return visible;
    return prIRectIntersect(visible, deviceBox(w, g.bounds));
}

// Pushes a group whose transform rotates or skews its space into a layer in its own space, at `resolution` of the
// device's resolution. What lies beneath the group is resampled into that space. Returns false if the group covers
// nothing there.
bool pushLocalGroup(Walk& w, const PrDrawCommand& c, GroupFrame& frame, PrIRect visible, float resolution) {
    const PrDrawGroup& g = *c.group;
    const PrDrawTransform x = w.transforms.last();
    PrAffine2 inverse{};
    if (!prAffine2Invert(x.affine, &inverse))
        return false;
    PrMatrix3 deviceToUser = prMatrix3FromAffine2(inverse);
    // The area of the group's space that shows, at the device's scale.
    PrRect area = prRectIsEmpty(g.bounds) ? PrRect{} : g.bounds;
    PrRect shown = prAffine2MapRect(inverse, prRectFromIRect(visible));
    area = prRectIsEmpty(area) ? shown : prRectIntersect(area, shown);
    float scale = prAffine2MaxScale(x.affine) * resolution;
    if (prRectIsEmpty(area) || !(scale > 0))
        return false;
    frame.local = true;
    frame.toLocal = prAffine2Scale(PrVec2{scale, scale});
    PrMatrix3 userToLocal = prMatrix3FromAffine2(frame.toLocal);
    PrMatrix3 deviceToLocal = prMatrix3Concat(&deviceToUser, &userToLocal);
    if (!prMatrix3Invert(&deviceToLocal, &frame.toDevice))
        return false;
    PrDrawTransform local = prDrawTransformFromAffine2(frame.toLocal);
    PrIRect box = prIRectRoundOut(prAffine2MapRect(frame.toLocal, area));
    int32_t reach = g.filter != nullptr ? filterReach(g.filter, local) : 0;
    if (g.backdrop != nullptr) {
        int32_t backdropReach = filterReach(g.backdrop, local);
        reach = backdropReach > reach ? backdropReach : reach;
    }
    box = prIRectOutset(box, g.filter != nullptr ? filterReach(g.filter, local) : 0);
    if (!prRectIsEmpty(g.bounds))
        box = prIRectIntersect(box, prIRectRoundOut(prAffine2MapRect(frame.toLocal, g.bounds)));
    if (prIRectIsEmpty(box))
        return false;
    // What lies beneath, resampled into the group's space.
    uint32_t under = 0;
    if (g.backdrop != nullptr && frame.under != 0)
        under = resampledUnder(w, frame.under, frame.toDevice, prIRectOutset(box, reach));
    uint32_t backdrop = under != 0 ? runFilter(w, g.backdrop, under, under, box, local, g.bounds) : 0;
    frame.transform = local;
    frame.under = under != 0 ? uint32_t(&layerOf(w, under) - w.b->layers.data) + 1 : 0;
    enterLayer(w, box, false);
    frame.layer = w.layers.last();
    w.groups.push(frame);
    w.knockouts.last() = g.knockout;
    w.transforms.push(local);
    if (backdrop != 0) {
        float color[4];
        FragmentPaint r = layerPaint(w, 1, color);
        compositeLayer(w, backdrop, box, r, color);
    }
    return true;
}

// Pushes a projected group into a layer that holds the visible part of its plane, at the projection's scale. The layer
// starts with the group's backdrop, which reads what lies beneath the plane through the projection. Returns false if
// the group draws nothing, where prDrawProjectionPlace places nothing.
bool pushProjectedGroup(Walk& w, GroupFrame& frame, PrIRect visible) {
    const PrDrawGroup& g = *frame.group;
    const PrDrawTransform& x = w.transforms.last();
    if (prIRectIsEmpty(visible) || !prDrawProjectionPlace(g.projection, x.affine, g.bounds, prRectFromIRect(visible),
            float(kListTextureMaxSize), &frame.projection))
        return false;
    float scale = frame.projection.scale;
    frame.toLocal = prAffine2Scale(PrVec2{scale, scale});
    PrIRect box = prIRectRoundOut(prAffine2MapRect(frame.toLocal, frame.projection.area));
    if (prIRectIsEmpty(box))
        return false;
    frame.local = true;
    frame.projected = true;
    frame.toDevice = frame.projection.layerToTarget;
    frame.transform = prDrawTransformFromAffine2(frame.toLocal);
    frame.beneath = frame.under;
    frame.under = 0;
    uint32_t backdrop = 0;
    if ((g.backdrop != nullptr || g.filter != nullptr) && frame.beneath != 0) {
        uint32_t under = resampledUnder(w, frame.beneath, frame.toDevice, box);
        frame.under = uint32_t(&layerOf(w, under) - w.b->layers.data) + 1;
        if (g.backdrop != nullptr)
            backdrop = runFilter(w, g.backdrop, under, under, box, frame.transform, g.bounds);
    }
    enterLayer(w, box, false);
    w.tracks.last() = true;
    frame.layer = w.layers.last();
    w.groups.push(frame);
    w.knockouts.last() = g.knockout;
    w.transforms.push(frame.transform);
    if (backdrop != 0) {
        float color[4];
        FragmentPaint r = layerPaint(w, 1, color);
        compositeLayer(w, backdrop, box, r, color);
    }
    return true;
}

// Pushes a group whose contents draw nothing that shows. Its contents draw into nothing.
void pushHiddenGroup(Walk& w, GroupFrame frame) {
    Clip nothing = clipOf(w);
    nothing.box = PrIRect{0, 0, 0, 0};
    w.clips.push(nothing);
    w.opacities.push(0);
    frame.hidden = true;
    w.groups.push(frame);
}

// Returns the innermost group that draws into a layer of the host's, or null.
GroupFrame* PR_NULLABLE hostGroupOf(Walk& w) {
    for (uint32_t i = w.groups.count; i-- > 0;) {
        if (w.groups.data[i].host)
            return &w.groups.data[i];
    }
    return nullptr;
}

// Pushes a group that draws into the host's texture, which keeps what it held. The group's bounds map onto the
// texture's texels, and its contents draw there in that space. Returns false if the group covers nothing.
bool pushHostGroup(Walk& w, GroupFrame& frame) {
    const PrDrawGroup& g = *frame.group;
    const PrDrawGroupLayer& layer = *g.layer;
    PrAffine2 inverse{};
    float width = prRectWidth(g.bounds);
    float height = prRectHeight(g.bounds);
    if (layer.extent.x == 0 || layer.extent.y == 0 || !(width > 0) || !(height > 0) ||
            !prAffine2Invert(w.transforms.last().affine, &inverse))
        return false;
    // The group's space to the texture's texels.
    float sx = float(layer.extent.x) / width;
    float sy = float(layer.extent.y) / height;
    frame.toLocal = PrAffine2{sx, 0, 0, sy, -g.bounds.left * sx, -g.bounds.top * sy};
    PrMatrix3 deviceToUser = prMatrix3FromAffine2(inverse);
    PrMatrix3 userToLocal = prMatrix3FromAffine2(frame.toLocal);
    PrMatrix3 deviceToLocal = prMatrix3Concat(&deviceToUser, &userToLocal);
    if (!prMatrix3Invert(&deviceToLocal, &frame.toDevice))
        return false;
    frame.local = true;
    frame.host = true;
    frame.drawn = drawSharedClaimLayer(layer);
    frame.transform = prDrawTransformFromAffine2(frame.toLocal);
    frame.under = 0;
    endPass(w);
    w.b->hostTextures.push(layer.texture);
    uint32_t texture = ++w.f->nextTexture;
    uint32_t index = w.b->layers.count + 1;
    w.f->layerIndex.insert(texture, w.b->layers.count);
    PrIRect box{0, 0, int32_t(layer.extent.x), int32_t(layer.extent.y)};
    w.b->layers.push(FragmentLayer{
        .texture = texture,
        .extent = layer.extent,
        .precise = w.extended,
        .exact = true,
        .hostTexture = w.b->hostTextures.count,
    });
    w.layers.push(index);
    w.knockouts.push(g.knockout);
    w.tracks.push(false);
    w.drawn.push(PrIRect{0, 0, 0, 0});
    // The texture keeps what it held: its pass loads it.
    beginPass(w, index, false);
    w.clips.push(clipOfBox(w, box));
    w.opacities.push(1);
    frame.layer = index;
    w.groups.push(frame);
    w.transforms.push(frame.transform);
    return true;
}

// Composites the host's texture of a group over the group's bounds, through the transform in force where the group was
// pushed, sampled as its layer says.
void popHostGroup(Walk& w, const GroupFrame& frame, uint32_t texture) {
    const PrDrawGroup& g = *frame.group;
    PrDrawComposite composite = w.composite;
    PrDrawBlendMode blendMode = w.blendMode;
    w.composite = frame.composite;
    w.blendMode = frame.blendMode;
    float color[4];
    FragmentPaint r = layerPaint(w, prF32Clamp(g.opacity, 0, 1) * opacityOf(w), color);
    PrMatrix3 deviceToLocal{};
    if (prMatrix3Invert(&frame.toDevice, &deviceToLocal)) {
        placeLayer(w, r, layerOf(w, texture), deviceToLocal);
        // The bounds cover the texture whole, so its edge texels extend to the edges instead of fading.
        r.sampling = g.layer->sampling;
        r.tileX = PrDrawExtend_Pad;
        r.tileY = PrDrawExtend_Pad;
        Ink ink = inkOf(w, r, color);
        if (prDrawCompositeIsUnbounded(frame.composite)) {
            drawSolid(w, clipOf(w).box, ink);
        } else {
            prPathClear(w.f->region);
            prPathAddRect(w.f->region, g.bounds);
            fillPath(w, w.f->region, PrPathFillRule_NonZero, true, ink);
        }
    }
    w.composite = composite;
    w.blendMode = blendMode;
}

// Pushes a group. A group with an effect draws its contents into a layer over the area that they may cover, clipped
// only by that area. With a filter, the area reaches as far as the filter may move what it reads. With a backdrop, the
// layer starts with what lies beneath it, filtered and clipped in the same way as the group. The layer composites when
// the group pops.
void pushGroup(Walk& w, const PrDrawCommand& c) {
    const PrDrawGroup& g = *c.group;
    GroupFrame frame{};
    frame.composite = c.composite;
    frame.blendMode = c.blendMode;
    frame.group = &g;
    frame.transform = w.transforms.last();
    frame.under = w.layers.last();
    if (!prDrawGroupHasEffect(&g)) {
        // Contents of a revision that the host's texture already holds have been drawn into it, and contents past the
        // layer's revision are not claimed.
        if (GroupFrame* host = g.revision != 0 ? hostGroupOf(w) : nullptr) {
            if (g.revision <= host->drawn || g.revision > host->group->layer->revision) {
                pushHiddenGroup(w, frame);
                return;
            }
        }
        w.groups.push(frame);
        w.opacities.push(opacityOf(w));
        return;
    }
    if (g.layer != nullptr) {
        if (!pushHostGroup(w, frame))
            pushHiddenGroup(w, frame);
        return;
    }
    PrIRect visible = prIRectIntersect(clipOf(w).box, targetBox(w));
    if (g.projection != nullptr) {
        if (!pushProjectedGroup(w, frame, visible))
            pushHiddenGroup(w, frame);
        return;
    }
    const PrDrawTransform& x = w.transforms.last();
    bool turned = !(x.affine.b == 0 && x.affine.c == 0);
    // A filter that reaches too far draws the group at a lower resolution, in its own space.
    bool filters = g.filter != nullptr || g.backdrop != nullptr;
    float resolution = 1;
    if (filters) {
        int32_t reach = g.filter != nullptr ? filterReach(g.filter, x) : 0;
        if (g.backdrop != nullptr) {
            int32_t backdropReach = filterReach(g.backdrop, x);
            reach = backdropReach > reach ? backdropReach : reach;
        }
        resolution = prDrawLayerResolution(float(reach));
    }
    if (filters && (turned || resolution < 1) && pushLocalGroup(w, c, frame, visible, resolution))
        return;
    PrIRect box = groupBox(w, g, visible);
    if (g.filter != nullptr && !prIRectIsEmpty(box))
        box = groupBox(w, g, prIRectOutset(box, filterReach(g.filter, w.transforms.last())));
    if (prIRectIsEmpty(box)) {
        pushHiddenGroup(w, frame);
        return;
    }
    uint32_t backdrop = 0;
    Clip outer = clipOf(w);
    if (g.backdrop != nullptr && frame.under != 0) {
        uint32_t under = underOf(w, frame, box);
        backdrop = runFilter(w, g.backdrop, under, under, box, w.transforms.last(), g.bounds);
    }
    enterLayer(w, box, false);
    frame.layer = w.layers.last();
    w.groups.push(frame);
    w.knockouts.last() = g.knockout;
    if (backdrop != 0) {
        outer.box = prIRectIntersect(outer.box, box);
        w.clips.push(outer);
        float color[4];
        FragmentPaint r = layerPaint(w, 1, color);
        compositeLayer(w, backdrop, box, r, color);
        w.clips.pop();
    }
}

// Draws a group's mask into its own layer over the box, and returns the layer's texture. The mask is its brush times
// its color, filling the group's bounds.
uint32_t drawGroupMask(Walk& w, const GroupFrame& frame, PrIRect box) {
    const PrDrawMask& mask = *frame.group->mask;
    uint32_t texture = enterLayer(w, box, false);
    w.transforms.push(frame.transform);
    PrRect area = frame.group->bounds;
    PrAffine2 fromDevice{};
    if (prRectIsEmpty(area) && prAffine2Invert(frame.transform.affine, &fromDevice))
        area = prAffine2MapRect(fromDevice, PrRect{
            float(box.left), float(box.top), float(box.right),
            float(box.bottom),
        });
    if (!prRectIsEmpty(area)) {
        PrDrawCommand fill{};
        fill.kind = PrDrawCommandKind_Rect;
        fill.antialias = true;
        fill.color = PrColor{1, 1, 1, mask.brush.opacity};
        fill.brush = &mask.brush;
        fill.rect = area;
        drawShape(w, fill);
    }
    w.transforms.pop();
    leaveLayer(w);
    return texture;
}

// Composites a projected group's layer over `box` where the part of its plane within `drawn` lands, through the record,
// whose compositing is set, times the color. Each target pixel samples the layer at its point of the plane, from the
// mip level that its footprint selects.
void compositeProjected(Walk& w, const GroupFrame& frame, uint32_t texture, PrIRect box, PrIRect drawn,
        FragmentPaint& r, const float color[4]) {
    mipmapLayer(w, texture);
    placeLayer(w, r, layerOf(w, texture), frame.projection.targetToLayer);
    r.sampling = PrDrawSampling_Mipmap;
    r.tile = prRectFromIRect(box);
    // The quad cuts the layer's edges, so the samples extend the edge texels instead of fading them.
    r.tileX = PrDrawExtend_Pad;
    r.tileY = PrDrawExtend_Pad;
    Ink ink = inkOf(w, r, color);
    if (prDrawCompositeIsUnbounded(frame.composite)) {
        drawSolid(w, clipOf(w).box, ink);
        return;
    }
    PrRect plane = prRectIntersect(prAffine2MapRect(frame.toLocal, frame.projection.area), prRectFromIRect(drawn));
    prPathPolylineClear(&w.f->landed);
    if (prRectIsEmpty(plane))
        return;
    prDrawProjectionQuad(&frame.projection, plane, &w.f->landed);
    fillDevice(w, w.f->landed, PrPathFillRule_NonZero, true, ink);
}

// Filters and masks a group's layer in its own space, then composites it through the rest of its transform.
void popLocalGroup(Walk& w, const GroupFrame& frame, uint32_t texture, PrIRect box, PrIRect drawn) {
    const PrDrawGroup& g = *frame.group;
    uint32_t result = texture;
    if (g.filter != nullptr) {
        uint32_t under = frame.under != 0 ? w.b->layers.data[frame.under - 1].texture : 0;
        result = runFilter(w, g.filter, texture, under, box, frame.transform, g.bounds);
        drawn = box;
    }
    if (g.mask != nullptr) {
        uint32_t mask = drawGroupMask(w, frame, box);
        uint32_t maskMode = FragmentMask_Luminance;
        if (g.mask->mode == PrDrawMaskMode_Alpha)
            maskMode = FragmentMask_Alpha;
        else if (g.mask->space == PrColorSpace_SrgbLinear)
            maskMode = FragmentMask_LinearLuminance;
        if (g.mask->invert)
            maskMode |= FragmentMaskFlag_Invert;
        uint32_t masked = enterLayer(w, box, false);
        float color[4];
        FragmentPaint r = layerPaint(w, 1, color);
        r.groupMask = maskMode;
        r.groupMaskTexture = tableOf(w, mask);
        compositeLayer(w, result, box, r, color);
        leaveLayer(w);
        result = masked;
    }
    // Composites through the parent's clips, with the group's operator and blend mode, over the layer's area on the
    // device.
    PrDrawComposite composite = w.composite;
    PrDrawBlendMode blendMode = w.blendMode;
    w.composite = frame.composite;
    w.blendMode = frame.blendMode;
    float color[4];
    FragmentPaint r = layerPaint(w, prF32Clamp(g.opacity, 0, 1) * opacityOf(w), color);
    PrMatrix3 deviceToLocal{};
    if (frame.projected) {
        compositeProjected(w, frame, result, box, drawn, r, color);
    } else if (prMatrix3Invert(&frame.toDevice, &deviceToLocal)) {
        placeLayer(w, r, layerOf(w, result), deviceToLocal);
        Ink ink = inkOf(w, r, color);
        if (prDrawCompositeIsUnbounded(frame.composite)) {
            drawSolid(w, clipOf(w).box, ink);
        } else {
            // The layer's area in the group's space, under the transform in force where the group was pushed.
            float inverse = 1 / frame.toLocal.a;
            prPathClear(w.f->region);
            prPathAddRect(w.f->region, PrRect{float(box.left) * inverse, float(box.top) * inverse,
                float(box.right) * inverse, float(box.bottom) * inverse});
            fillPath(w, w.f->region, PrPathFillRule_NonZero, true, ink);
        }
    }
    w.composite = composite;
    w.blendMode = blendMode;
}

void popGroup(Walk& w) {
    GroupFrame frame = w.groups.data[--w.groups.count];
    if (frame.layer == 0) {
        w.opacities.pop();
        if (prDrawGroupHasEffect(frame.group) || frame.hidden)
            w.clips.pop();
        return;
    }
    const FragmentLayer layer = w.b->layers.data[frame.layer - 1];
    PrIRect box = prIRectAt(layer.origin, layer.extent);
    if (frame.local)
        w.transforms.pop();
    PrIRect drawn = leaveLayer(w);
    if (frame.host) {
        popHostGroup(w, frame, layer.texture);
        return;
    }
    if (frame.local) {
        popLocalGroup(w, frame, layer.texture, box, drawn);
        return;
    }
    const PrDrawGroup& g = *frame.group;
    uint32_t result = layer.texture;
    if (g.filter != nullptr) {
        uint32_t under = underOf(w, frame, box);
        result = runFilter(w, g.filter, layer.texture, under, box, frame.transform, g.bounds);
    }
    uint32_t mask = 0;
    uint32_t maskMode = FragmentMask_None;
    if (g.mask != nullptr) {
        mask = drawGroupMask(w, frame, box);
        if (g.mask->mode == PrDrawMaskMode_Alpha)
            maskMode = FragmentMask_Alpha;
        else if (g.mask->space == PrColorSpace_SrgbLinear)
            maskMode = FragmentMask_LinearLuminance;
        else
            maskMode = FragmentMask_Luminance;
        if (g.mask->invert)
            maskMode |= FragmentMaskFlag_Invert;
    }
    // Composites through the parent's clips, with the group's operator and blend mode. An operator that changes what
    // lies outside the contents covers the whole clip.
    PrDrawComposite composite = w.composite;
    PrDrawBlendMode blendMode = w.blendMode;
    w.composite = frame.composite;
    w.blendMode = frame.blendMode;
    float color[4];
    FragmentPaint r = layerPaint(w, prF32Clamp(g.opacity, 0, 1) * opacityOf(w), color);
    r.groupMask = maskMode;
    r.groupMaskTexture = tableOf(w, mask);
    compositeLayer(w, result, prDrawCompositeIsUnbounded(frame.composite) ? clipOf(w).box : box, r, color);
    w.composite = composite;
    w.blendMode = blendMode;
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

// Returns the rectangle of the pixels whose centers lie inside the given rectangle.
PrRect centersOf(PrRect r) {
    return PrRect{prF32Ceil(r.left - 0.5f), prF32Ceil(r.top - 0.5f), prF32Ceil(r.right - 0.5f), prF32Ceil(r.bottom - 0.5f)};
}

// Pushes a clip. A rectangle that stays axis-aligned narrows the clip's rectangle, and a rounded or turned rectangle
// adds a shape that clip records evaluate. Any other clip draws its coverage, under the clips already in force, into a
// mask layer. The mask layer then replaces those clips for what draws under it.
void pushClip(Walk& w, const PrDrawCommand& c) {
    Clip clip = clipOf(w);
    clip.record = 0;
    const PrDrawTransform& x = w.transforms.last();
    FragmentClipShape shape{};
    PrRect rect{};
    bool analytic = clipShapeOf(w, c, &shape, &rect);
    if (!analytic) {
        prPathClear(w.f->shape);
        prDrawShapePath(&c, w.f->shape);
        if (c.kind == PrDrawCommandKind_PushClipTargetRect) {
            rect = c.rect;
        } else {
            rect = prAffine2MapRect(x.affine, prPathTightBounds(w.f->shape));
        }
    }
    if (prDrawTransformDrawsNothing(&x, &c) || prRectIsEmpty(rect)) {
        clip.box = PrIRect{0, 0, 0, 0};
        clip.whole = false;
        w.clips.push(clip);
        return;
    }
    bool square = true;
    for (PrVec2 radius : shape.shape.radii)
        square = square && radius.x <= 0 && radius.y <= 0;
    bool aligned = c.kind == PrDrawCommandKind_PushClipTargetRect || prAffine2IsAxisAligned(x.affine);
    if (analytic && square && aligned) {
        PrRect device = c.antialias ? rect : centersOf(rect);
        PrIRect into = targetBox(w);
        clip.whole = clip.whole && device.left <= float(into.left) && device.top <= float(into.top) &&
            device.right >= float(into.right) && device.bottom >= float(into.bottom);
        clip.rect = prRectIntersect(clip.rect, device);
        clip.box = prIRectIntersect(clip.box, prIRectRoundOut(clip.rect));
        w.clips.push(clip);
        return;
    }
    clip.box = prIRectIntersect(clip.box, prIRectRoundOut(rect));
    clip.whole = false;
    if (analytic) {
        // The walk's clip shapes are truncated to end with the shapes of the clips in force, and the new shape follows.
        w.clipShapes.count = clip.first + clip.count;
        w.clipShapes.push(w.b->shapes.count);
        w.b->shapes.push(shape);
        ++clip.count;
    } else if (!prIRectIsEmpty(clip.box)) {
        uint32_t mask = enterLayer(w, clip.box, true);
        float color[4];
        FragmentPaint r = layerPaint(w, 1, color);
        r.composite = PrDrawComposite_SourceOver;
        r.blendMode = PrDrawBlendMode_Normal;
        Ink ink = inkOf(w, r, color);
        if (c.kind == PrDrawCommandKind_PushClipTargetRect) {
            prPathClear(w.f->shape);
            prPathAddRect(w.f->shape, c.rect);
            prPathPolylineClear(&w.f->polyline);
            prPathFlatten(w.f->shape, prAffine2Identity(), kTolerance, PrPathFlattening_Inside, &w.f->polyline);
            fillDevice(w, w.f->polyline, PrPathFillRule_NonZero, c.antialias, ink);
        } else {
            prPathClear(w.f->shape);
            prDrawShapePath(&c, w.f->shape);
            fillPath(w, w.f->shape, c.kind == PrDrawCommandKind_PushClipPath ? c.rule : PrPathFillRule_NonZero,
                c.antialias, ink);
        }
        leaveLayer(w);
        // The mask holds every clip pushed so far.
        clip.rect = PrRect{float(clip.box.left), float(clip.box.top), float(clip.box.right), float(clip.box.bottom)};
        clip.first = w.clipShapes.count;
        clip.count = 0;
        clip.mask = mask;
    }
    w.clips.push(clip);
}

void popClip(Walk& w) {
    w.clips.pop();
}

void drawCommand(Walk& w, const PrDrawCommand& c);

// Draws a painting command with its operator and blend mode, which apply where its shape covers.
void drawPainting(Walk& w, const PrDrawCommand& c) {
    w.composite = c.composite;
    w.blendMode = c.blendMode;
    drawCommand(w, c);
    w.composite = PrDrawComposite_SourceOver;
    w.blendMode = PrDrawBlendMode_Normal;
}

void drawCommand(Walk& w, const PrDrawCommand& c) {
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
        case PrDrawCommandKind_Glyphs:
            drawGlyphs(w, c);
            break;
        default:
            drawShape(w, c);
            break;
    }
}

// Returns true if nothing that an embedded list draws reaches the current clip, so that playing it changes nothing.
bool missesClip(const Walk& w, const PrDrawList* embedded) {
    const PrDrawTransform& x = w.transforms.last();
    return !x.degenerate && prDrawListMisses(embedded, x.affine, prRectFromIRect(clipOf(w).box));
}

// The counts of the frame's arrays, to tell which ones an embedded list's records added to.
struct Counts {
    uint32_t instances;
    uint32_t values;
    uint32_t coverage;
    uint32_t tileData;
    uint32_t paints;
    uint32_t clips;
    uint32_t clipShapes;
    uint32_t shapes;
    uint32_t pieces;
    uint32_t maps;
    uint32_t mapData;
    uint32_t mapIndices;
    uint32_t rampTables;
    uint32_t rampCells;
    uint32_t rampPieces;
    uint32_t rampColors;
    uint32_t textures;
    uint32_t hostTextures;
    uint32_t layers;
    uint32_t passes;
    uint32_t filters;
    uint32_t filterData;
    uint32_t requests;
    uint32_t stacked;
};

// Returns the counts of the frame's arrays.
Counts countsOf(const Walk& w) {
    const FrameStorage& b = *w.b;
    return Counts{
        b.instances.count,    b.values.count,       b.coverage.count,     b.tileData.count,
        b.paints.count,       b.clips.count,        b.clipShapes.count,   b.shapes.count,
        b.pieces.count,       b.maps.count,         b.mapData.count,      b.mapIndices.count,
        b.rampTables.count,   b.rampCells.count,    b.rampPieces.count,   b.rampColors.count,
        b.textures.count,     b.hostTextures.count, b.layers.count,       b.passes.count,
        b.filters.count,      b.filterData.count,   b.requests.count,     w.stacked.count,
    };
}

// Writes the walk's state as an embed's key. Returns false where no records are kept: under a clip of shapes or a mask.
bool keyOf(const Walk& w, PreparedKey* out) {
    const Clip& c = clipOf(w);
    if (c.count != 0 || c.mask != 0)
        return false;
    *out = PreparedKey{
        .transform = w.transforms.last().affine,
        .clipBox = c.box,
        .clipRect = c.rect,
        .opacity = opacityOf(w),
        .layer = w.layers.last(),
        .composite = w.composite,
        .blendMode = w.blendMode,
        .knockout = w.knockouts.last(),
    };
    return true;
}

// Returns true if two keys are the same state.
bool sameKey(const PreparedKey& a, const PreparedKey& b) {
    bool placed = prMemEquals(&a.transform, &b.transform, sizeof a.transform) &&
        prMemEquals(&a.clipBox, &b.clipBox, sizeof a.clipBox) &&
        prMemEquals(&a.clipRect, &b.clipRect, sizeof a.clipRect);
    return placed && a.opacity == b.opacity && a.layer == b.layer && a.composite == b.composite &&
        a.blendMode == b.blendMode && a.knockout == b.knockout;
}

// Frees the kept records of every list, and makes the table `count` entries, a power of two.
void resizePrepared(Fragment* f, uint32_t count) {
    for (uint32_t i = 0; i < f->prepared.count; ++i) {
        PreparedList& p = f->prepared.data[i];
        p.instances.reset();
        p.values.reset();
        p.paints.reset();
        p.clips.reset();
        p.tables.reset();
    }
    f->prepared.reset();
    for (uint32_t i = 0; i < count; ++i)
        f->prepared.push(PreparedList{});
}

// Returns true if the coverage of an instance with `flags` reads a record from the values at its `data`.
bool readsValues(uint32_t flags) {
    uint32_t kind = flags & FragmentInstanceFlag_Coverage;
    return kind >= FragmentCoverage_Rect && kind <= FragmentCoverage_Shadow;
}

// Returns true if an instance's `data` names an atlas page in the frame's textures.
bool readsAtlas(uint32_t flags) {
    return (flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_Atlas;
}

// Returns true if the list, under `transform`, lies inside the clip's box.
bool liesInside(const Walk& w, const PrDrawList* list, PrAffine2 transform) {
    PrRect bounds = prDrawListBounds(list);
    if (prDrawListWritesPixels(list) || !prF32IsFinite(bounds.left) || !prF32IsFinite(bounds.right) ||
            !prF32IsFinite(bounds.top) || !prF32IsFinite(bounds.bottom))
        return false;
    PrIRect box = prIRectRoundOut(prAffine2MapRect(transform, bounds));
    PrIRect clip = clipOf(w).box;
    return box.left >= clip.left && box.top >= clip.top && box.right <= clip.right && box.bottom <= clip.bottom;
}

// Returns true if `key` is `kept` moved by whole target pixels, which it sets `offset` to.
bool movedKey(const PreparedKey& kept, const PreparedKey& key, PrVec2* offset) {
    PreparedKey moved = kept;
    moved.transform.e = key.transform.e;
    moved.transform.f = key.transform.f;
    if (!sameKey(moved, key))
        return false;
    float dx = key.transform.e - kept.transform.e;
    float dy = key.transform.f - kept.transform.f;
    if (dx != prF32Round(dx) || dy != prF32Round(dy))
        return false;
    *offset = PrVec2{dx, dy};
    return true;
}

// Copies the kept records of `p` into the frame, moved by `offset` target pixels.
void copyPrepared(Walk& w, const PreparedList& p, PrVec2 offset = PrVec2{0, 0}) {
    FrameStorage& b = *w.b;
    uint32_t values = b.values.count;
    uint32_t paints = b.paints.count;
    uint32_t clips = b.clips.count;
    b.values.append(p.values.span());
    b.paints.append(p.paints.span());
    b.clips.append(p.clips.span());
    bool moved = offset.x != 0 || offset.y != 0;
    if (moved) {
        for (uint32_t i = clips; i < b.clips.count; ++i) {
            PrRect& r = b.clips.data[i].rect;
            r = PrRect{r.left + offset.x, r.top + offset.y, r.right + offset.x, r.bottom + offset.y};
        }
    }
    uint32_t outer = 0;
    bool outerMade = false;
    uint32_t tables[16];
    for (uint32_t t = 0; t < p.tables.count; ++t)
        tables[t] = tableEntry(w, p.tables.data[t], 0, PrColor16{});
    uint32_t at = b.instances.count;
    b.instances.reserve(at + p.instances.count);
    b.instances.count = at + p.instances.count;
    FragmentInstance* out = b.instances.data + at;
    for (uint32_t i = 0; i < p.instances.count; ++i) {
        FragmentInstance n = p.instances.data[i];
        if (readsValues(n.flags))
            n.data += values;
        if (n.paint != 0)
            n.paint += paints;
        if (n.clip == kOuterClip) {
            if (!outerMade) {
                outer = clipRecord(w);
                outerMade = true;
            }
            n.clip = outer;
        } else if (n.clip != 0) {
            n.clip += clips;
        }
        if (readsAtlas(n.flags))
            n.data = tables[n.data - 1];
        if (moved) {
            if ((n.flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_Triangle) {
                for (PrVec2& corner : n.corners)
                    corner = corner + offset;
            } else {
                n.transform.e += offset.x;
                n.transform.f += offset.y;
            }
            if ((n.flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_Rect) {
                float* r = b.values.data + n.data;
                r[0] += offset.x;
                r[1] += offset.y;
                r[2] += offset.x;
                r[3] += offset.y;
            }
        }
        out[i] = n;
    }
    for (uint32_t page = 0; page < 64; ++page) {
        if ((p.pages >> page & 1) != 0)
            prDrawAtlasKeepPage(w.f->atlas, page);
    }
    if (w.tracks.last()) {
        PrIRect drawn = p.drawn;
        if (!prIRectIsEmpty(drawn))
            drawn = PrIRect{
                drawn.left + int32_t(offset.x), drawn.top + int32_t(offset.y),
                drawn.right + int32_t(offset.x), drawn.bottom + int32_t(offset.y),
            };
        w.drawn.last() = prIRectUnion(w.drawn.last(), drawn);
    }
}

// Keeps the records that the frame added since `before` for the list, if they can be copied to a later frame. Returns
// false if they cannot.
bool keepPrepared(Walk& w, PreparedList& p, const Counts& before, const Counts& after) {
    if (after.coverage != before.coverage || after.tileData != before.tileData || after.clipShapes != before.clipShapes ||
            after.shapes != before.shapes || after.pieces != before.pieces || after.maps != before.maps ||
            after.mapData != before.mapData || after.mapIndices != before.mapIndices ||
            after.rampTables != before.rampTables || after.rampCells != before.rampCells ||
            after.rampPieces != before.rampPieces || after.rampColors != before.rampColors ||
            after.hostTextures != before.hostTextures || after.layers != before.layers ||
            after.passes != before.passes || after.filters != before.filters || after.filterData != before.filterData ||
            after.requests != before.requests || after.stacked != before.stacked)
        return false;
    const FrameStorage& b = *w.b;
    p.instances.clear();
    p.values.clear();
    p.paints.clear();
    p.clips.clear();
    p.tables.clear();
    p.pages = 0;
    for (uint32_t i = before.paints; i < after.paints; ++i) {
        const FragmentPaint& r = b.paints.data[i];
        if (r.kind != FragmentPaintKind_Solid || r.map != 0 || r.groupMaskTexture != 0 || r.texture != 0)
            return false;
    }
    for (uint32_t i = before.clips; i < after.clips; ++i) {
        if (b.clips.data[i].shapeCount != 0 || b.clips.data[i].mask != 0)
            return false;
    }
    for (uint32_t i = before.instances; i < after.instances; ++i) {
        FragmentInstance n = b.instances.data[i];
        if (readsValues(n.flags))
            n.data -= before.values;
        if (n.paint != 0) {
            if (n.paint <= before.paints)
                return false;
            n.paint -= before.paints;
        }
        if (n.clip != 0)
            n.clip = n.clip > before.clips ? n.clip - before.clips : kOuterClip;
        if (readsAtlas(n.flags)) {
            const FragmentTexture& e = b.textures.data[n.data - 1];
            if (e.under != 0 || e.hostTexture != 0)
                return false;
            uint32_t t = 0;
            while (t < p.tables.count && p.tables.data[t] != e.texture)
                ++t;
            if (t == p.tables.count) {
                if (t == 16)
                    return false;
                p.tables.push(e.texture);
            }
            n.data = t + 1;
            uint32_t page = 0;
            while (page < w.f->pageTextures.count && w.f->pageTextures.data[page] != e.texture)
                ++page;
            if (page == w.f->pageTextures.count || page >= 64)
                return false;
            p.pages |= uint64_t(1) << page;
        }
        p.instances.push(n);
    }
    p.values.append(b.values.data + before.values, after.values - before.values);
    p.paints.append(b.paints.data + before.paints, after.paints - before.paints);
    p.clips.append(b.clips.data + before.clips, after.clips - before.clips);
    return true;
}

// Plays an embedded list, from the records that an earlier build kept for it if they hold, and keeps its records for
// later builds where it can.
void embed(Walk& w, const PrDrawList* list) {
    Fragment* f = w.f;
    PreparedList* entry = nullptr;
    uint64_t id = 0;
    PreparedKey key{};
    if (f->embeds == PrDrawFragmentEmbeds_Keep && f->prepared.count != 0) {
        id = prDrawListId(list);
        entry = &f->prepared.data[KeyIndex::hashOf(id) & (f->prepared.count - 1)];
        if ((entry->id == id && entry->never) || !keyOf(w, &key)) {
            entry = nullptr;
        } else if (entry->id == id && entry->build == f->build && !sameKey(entry->key, key)) {
            entry->never = true;
            entry = nullptr;
        }
    }
    if (entry == nullptr) {
        if (!missesClip(w, list))
            runCommands(w, prDrawListCommands(list), prDrawListCommandCount(list));
        return;
    }
    ++f->embedded;
    entry->build = f->build;
    if (entry->id == id && entry->kept && entry->clears == prDrawAtlasInfo(f->atlas).clears) {
        PrVec2 offset{};
        if (sameKey(entry->key, key) ||
                (entry->whole && movedKey(entry->key, key, &offset) && liesInside(w, list, key.transform))) {
            copyPrepared(w, *entry, offset);
            return;
        }
    }
    Counts before = countsOf(w);
    PrIRect drawnBefore{};
    if (w.tracks.last()) {
        drawnBefore = w.drawn.last();
        w.drawn.last() = PrIRect{};
    }
    if (!missesClip(w, list))
        runCommands(w, prDrawListCommands(list), prDrawListCommandCount(list));
    PrIRect drawn{};
    if (w.tracks.last()) {
        drawn = w.drawn.last();
        w.drawn.last() = prIRectUnion(drawnBefore, drawn);
    }
    // The walk may have kept records of the lists embedded within this one, in other entries, but the table does not
    // move. A list's records are kept the second time that it is walked in the same state, so that a list whose state
    // changes from frame to frame costs no copies.
    bool again = entry->id == id && !entry->kept && !entry->never && sameKey(entry->key, key);
    if (entry->id != id) {
        entry->id = id;
        entry->never = false;
    }
    entry->key = key;
    entry->kept = false;
    if (!again)
        return;
    entry->drawn = drawn;
    entry->whole = liesInside(w, list, key.transform);
    entry->clears = prDrawAtlasInfo(f->atlas).clears;
    entry->kept = keepPrepared(w, *entry, before, countsOf(w));
    entry->never = !entry->kept;
}

void runCommands(Walk& w, const PrDrawCommand* commands, uint32_t count) {
    PrDrawSnapped snapped;
    for (uint32_t i = 0; i < count; ++i) {
        const PrDrawCommand& c = *prDrawTransformSnap(&w.transforms.last(), &commands[i], &snapped);
        if (prDrawTransformDrawsNothing(&w.transforms.last(), &c)) {
            // A push still opens a scope that its pop closes.
            if (c.kind >= PrDrawCommandKind_PushClipRect && c.kind <= PrDrawCommandKind_PushClipPath) {
                Clip nothing = clipOf(w);
                nothing.box = PrIRect{0, 0, 0, 0};
                w.clips.push(nothing);
            } else if (c.kind == PrDrawCommandKind_PushGroup) {
                GroupFrame hidden{};
                hidden.composite = c.composite;
                hidden.blendMode = c.blendMode;
                hidden.group = c.group;
                hidden.transform = w.transforms.last();
                w.groups.push(hidden);
                w.opacities.push(0);
                if (prDrawGroupHasEffect(c.group)) {
                    Clip nothing = clipOf(w);
                    nothing.box = PrIRect{0, 0, 0, 0};
                    w.clips.push(nothing);
                }
            }
            continue;
        }
        switch (c.kind) {
            case PrDrawCommandKind_WritePixels:
                writePixels(w, c);
                break;
            case PrDrawCommandKind_Embed:
                w.transforms.push(prDrawTransformMoved(&w.transforms.last(), c.embed.offset));
                embed(w, c.embed.list);
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
                pushGroup(w, c);
                break;
            case PrDrawCommandKind_PopGroup:
                popGroup(w);
                break;
            case PrDrawCommandKind_PushMark:
            case PrDrawCommandKind_PopMark:
                break;
            default:
                drawPainting(w, c);
                break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Builds

// Adds the requests for the images that went unused too long, and points the requests to their texels.
void finishRequests(Walk& w) {
    Fragment* f = w.f;
    uint32_t size = w.f->atlasSize;
    for (uint32_t page = 0; page < prDrawAtlasPageCount(f->atlas); ++page) {
        uint32_t texture = pageTexture(w, page);
        PrIRect region{};
        bool changed = prDrawAtlasTakeChanges(f->atlas, page, &region);
        bool create = page >= f->pagesCreated;
        if (create)
            region = PrIRect{0, 0, int32_t(size), int32_t(size)};
        else if (!changed)
            continue;
        uint32_t left = uint32_t(region.left);
        uint32_t top = uint32_t(region.top);
        PrPixelView pageTexels = prDrawAtlasPageTexels(f->atlas, page);
        const uint8_t* texels = pageTexels.data + size_t(top) * pageTexels.stride + left;
        FragmentTextureOp op = create ? FragmentTextureOp_Create : FragmentTextureOp_Update;
        request(w, FragmentTextureRequest{
            .op = op,
            .format = PrPixelFormat_R8,
            .texture = texture,
            .origin = {left, top},
            .texels = PrPixelView{texels, pageTexels.stride, {prIRectWidth(region), prIRectHeight(region)}},
        });
    }
    f->pagesCreated = prDrawAtlasPageCount(f->atlas);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < f->images.count; ++i) {
        const CachedImage& image = f->images.data[i];
        if (f->build - image.lastUsed < kIdleBuilds) {
            f->images.data[kept++] = image;
            continue;
        }
        request(w, FragmentTextureRequest{
            .op = FragmentTextureOp_Destroy,
            .format = PrPixelFormat_Rgba8,
            .texture = image.texture,
        });
    }
    f->images.count = kept;
    f->imageIndex.rebuild(kept, [&](uint32_t i) { return imageKey(f->images.data[i].id, f->images.data[i].mipmapped); });
    for (uint32_t i = 0; i < w.b->pending.count; ++i) {
        const PendingTexels& pending = w.b->pending.data[i];
        w.b->requests.data[pending.request].texels.data = w.b->texels.data + pending.offset;
    }
}

void describe(const FrameStorage* b, uint32_t width, uint32_t height, FragmentFrame* out) {
    *out = FragmentFrame{};
    out->extent.x = width;
    out->extent.y = height;
    out->requests = b->requests.data;
    out->requestCount = b->requests.count;
    out->textures = b->textures.data;
    out->textureCount = b->textures.count;
    out->hostTextures = b->hostTextures.data;
    out->hostTextureCount = b->hostTextures.count;
    out->rampTables = b->rampTables.data;
    out->rampCells = b->rampCells.data;
    out->rampCount = b->rampTables.count / (kFragmentRampResolution + 1);
    out->rampPieces = b->rampPieces.data;
    out->rampPieceCount = b->rampPieces.count;
    out->rampColors = b->rampColors.data;
    out->rampColorCount = b->rampColors.count;
    out->instances = b->instances.data;
    out->instanceCount = b->instances.count;
    out->values = b->values.data;
    out->valueCount = b->values.count;
    out->coverage = b->coverage.data;
    out->coverageCount = b->coverage.count;
    out->tileData = b->tileData.data;
    out->tileDataCount = b->tileData.count;
    out->paints = b->paints.data;
    out->paintCount = b->paints.count;
    out->clips = b->clips.data;
    out->clipCount = b->clips.count;
    out->clipShapes = b->clipShapes.data;
    out->clipShapeCount = b->clipShapes.count;
    out->shapes = b->shapes.data;
    out->shapeCount = b->shapes.count;
    out->pieces = b->pieces.data;
    out->pieceCount = b->pieces.count;
    out->maps = b->maps.data;
    out->mapCount = b->maps.count;
    out->mapData = b->mapData.data;
    out->mapDataCount = b->mapData.count;
    out->mapIndices = b->mapIndices.data;
    out->mapIndexCount = b->mapIndices.count;
    out->filters = b->filters.data;
    out->filterCount = b->filters.count;
    out->filterData = b->filterData.data;
    out->filterDataCount = b->filterData.count;
    out->layers = b->layers.data;
    out->layerCount = b->layers.count;
    out->passes = b->passes.data;
    out->passCount = b->passes.count;
    out->beneath = b->beneath;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building frames

// Creates the state that builds share, for the options.
Fragment* fragmentCreate(const PrDrawFragmentOptions& o) {
    Fragment* f = allocZeroed<Fragment>();
    f->embeds = o.embeds;
    f->paths = o.paths;
    f->atlasSize = o.atlasSize < kPrDrawFragmentAtlasMinSize ? kPrDrawFragmentAtlasMinSize : o.atlasSize;
    PrDrawAtlasOptions atlas = prDrawAtlasOptionsDefault();
    atlas.size = f->atlasSize;
    f->atlas = prDrawAtlasCreate(&atlas);
    PrPathSparseRasterizerOptions sparse = prPathSparseRasterizerOptionsDefault();
    sparse.kind = o.paths == PrDrawFragmentPaths_Tiles ? PrPathSparseKind_Segments : PrPathSparseKind_Coverage;
    sparse.tileExtent.x = sparse.tileExtent.y = o.paths == PrDrawFragmentPaths_Tiles ? kSegmentTile : kStripTile;
    f->sparse = prPathSparseRasterizerCreate(&sparse);
    f->lists = prDrawGlyphCacheCreate(o.glyphDecoder);
    f->tessellator = prPathTessellatorCreate();
    f->shape = prPathCreate();
    f->region = prPathCreate();
    return f;
}

void fragmentDestroy(Fragment* f) {
    resizePrepared(f, 0);
    f->images.reset();
    f->imageIndex.reset();
    f->tableIndex.reset();
    f->layerIndex.reset();
    f->ramps.reset();
    f->rampIndex.reset();
    f->pageTextures.reset();
    f->cursors.reset();
    prDrawAtlasDestroy(f->atlas);
    prPathSparseRasterizerDestroy(f->sparse);
    prPathPolylineReset(&f->polyline);
    prDrawGlyphCacheDestroy(f->lists);
    prPathTessellatorDestroy(f->tessellator);
    prPathDestroy(f->shape);
    prPathDestroy(f->region);
    prPathPolylineReset(&f->centerline);
    PrPathPolyline* polylines[] = {&f->fill, &f->aligned, &f->strokeRegion, &f->combined, &f->cut, &f->landed};
    for (PrPathPolyline* polyline : polylines)
        prPathPolylineReset(polyline);
    prMemoryFree(f);
}

// Forgets every texture, for example after the host loses its device. The next build recreates the textures that it
// draws with.
void fragmentForgetResources(Fragment* f) {
    resizePrepared(f, f->prepared.count);
    f->images.clear();
    f->imageIndex.index.clear();
    prDrawAtlasClear(f->atlas);
    f->pageTextures.clear();
    f->pagesCreated = 0;
}

void storageDestroy(FrameStorage* b) {
    b->instances.reset();
    b->values.reset();
    b->coverage.reset();
    b->tileData.reset();
    b->paints.reset();
    b->clips.reset();
    b->clipShapes.reset();
    b->shapes.reset();
    b->pieces.reset();
    b->maps.reset();
    b->mapData.reset();
    b->mapIndices.reset();
    b->rampTables.reset();
    b->rampCells.reset();
    b->rampPieces.reset();
    b->rampColors.reset();
    b->textures.reset();
    b->hostTextures.reset();
    b->layers.reset();
    b->passes.reset();
    b->filters.reset();
    b->filterData.reset();
    b->requests.reset();
    b->texels.reset();
    b->pending.reset();
    prMemoryFree(b);
}

// Builds the frame that draws the part of the list over the target into the storage, and describes it in `out`. A
// backdrop reads what lies beneath the target in the form that the target's `beneath` gives: a color that the frame
// draws itself, the host's texture in the frame's beneath layer, or the host's drawing at the start of the passes
// marked for it. Without it, what lies beneath is transparent. The frame stays valid until the storage's next build.
// The requests' texels stay valid until the next build with any storage.
void fragmentBuild(Fragment* f, const PrDrawList* list, const PrDrawPart& part, const PrDrawTarget& target,
        FrameStorage* b, FragmentFrame* out) {
    uint32_t width = target.extent.x;
    uint32_t height = target.extent.y;
    b->instances.clear();
    b->values.clear();
    b->coverage.clear();
    b->tileData.clear();
    b->paints.clear();
    b->clips.clear();
    b->clipShapes.clear();
    b->shapes.clear();
    b->pieces.clear();
    b->maps.clear();
    b->mapData.clear();
    b->mapIndices.clear();
    b->rampTables.clear();
    b->rampCells.clear();
    b->rampPieces.clear();
    b->rampColors.clear();
    b->textures.clear();
    b->hostTextures.clear();
    b->layers.clear();
    b->passes.clear();
    b->filters.clear();
    b->filterData.clear();
    b->requests.clear();
    b->texels.clear();
    b->pending.clear();
    b->pathFills = 0;
    b->beneath = 0;
    ++f->build;
    if (f->embeds == PrDrawFragmentEmbeds_Keep && (f->prepared.count == 0 || f->embedded * 4 > f->prepared.count)) {
        uint32_t count = f->prepared.count == 0 ? 1024 : f->prepared.count;
        while (f->embedded * 4 > count)
            count *= 2;
        resizePrepared(f, count);
    }
    f->embedded = 0;
    f->ramps.clear();
    f->rampIndex.clear();
    f->tableIndex.clear();
    f->layerIndex.index.clear();
    prDrawAtlasNext(f->atlas);
    prDrawGlyphCacheBegin(f->lists);
    Walk w{};
    w.f = f;
    w.b = b;
    w.target = PrIRect{0, 0, int32_t(width), int32_t(height)};
    w.extended = target.format == PrPixelFormat_RgbaF16;
    w.transforms.push(part.transform);
    w.clips.push(clipOfBox(w, w.target));
    w.opacities.push(1);
    w.layers.push(0);
    w.knockouts.push(false);
    w.tracks.push(false);
    w.drawn.push(PrIRect{0, 0, 0, 0});
    w.composite = PrDrawComposite_SourceOver;
    w.blendMode = PrDrawBlendMode_Normal;
    w.beneath = target.beneath;
    beginPass(w, 0, false);
    // A group that reads what lies beneath it reads what the list drew before it, over what the host provides. The
    // target cannot be read while preparing, so a list with such a group draws into a layer of its own, which then
    // draws over the target.
    const PrDrawCommand* commands = prDrawListCommands(list) + part.first;
    uint32_t count = part.count;
    bool root = width != 0 && height != 0 && prDrawListReadsBackdrop(list);
    uint32_t rootTexture = root ? enterLayer(w, w.target, false) : 0;
    w.root = root ? w.layers.last() : 0;
    if (root)
        w.tracks.last() = true;
    runCommands(w, commands, count);
    if (root) {
        leaveLayer(w);
        float color[4];
        FragmentPaint r = layerPaint(w, 1, color);
        compositeLayer(w, rootTexture, w.target, r, color);
    }
    endPass(w);
    finishRequests(w);
    prDrawGlyphCacheEnd(f->lists);
    w.transforms.reset();
    w.clips.reset();
    w.clipShapes.reset();
    w.opacities.reset();
    w.layers.reset();
    w.knockouts.reset();
    w.tracks.reset();
    w.drawn.reset();
    w.groups.reset();
    w.stacked.reset();
    w.rampColors.reset();
    describe(b, width, height, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// The renderer

// The renderer: what builds its frames, and its state on its device.
struct FragmentRenderer : PrDrawRenderer {
    PrGpuDevice* device;
    FragmentDevice* state;              // what the renderer keeps on its device
    Fragment* fragment;
};

// A frame: the data that the build produced, and the storage that it points into.
struct FragmentDrawFrame : PrDrawFrame {
    FragmentRenderer* renderer;
    FrameStorage* storage;
    FragmentFrame built;
};

PrGpuDevice* rendererDevice(const PrDrawRenderer* renderer) {
    return static_cast<const FragmentRenderer*>(renderer)->device;
}

void destroyFrame(PrDrawFrame* handle) {
    FragmentDrawFrame* frame = static_cast<FragmentDrawFrame*>(handle);
    FragmentRenderer* r = frame->renderer;
    fragmentDeviceReleaseFrame(r->state, &frame->built);
    storageDestroy(frame->storage);
    prMemoryFree(frame);
}

PrStatus prepare(PrDrawFrame* handle, const PrDrawList* list, const PrDrawPart* part, const PrDrawTarget* target) {
    FragmentDrawFrame* frame = static_cast<FragmentDrawFrame*>(handle);
    FragmentRenderer* r = frame->renderer;
    fragmentBuild(r->fragment, list, *part, *target, frame->storage, &frame->built);
    fragmentDevicePrepare(r->state, &frame->built, target->commands, target->format, target->beneath);
    return PrStatus_Ok;
}

void render(PrDrawFrame* handle, PrGpuPass* pass, PrIVec2 origin) {
    FragmentDrawFrame* frame = static_cast<FragmentDrawFrame*>(handle);
    FragmentRenderer* r = frame->renderer;
    fragmentDeviceRender(r->state, &frame->built, pass, origin);
}

void dropResources(PrDrawRenderer* renderer) {
    FragmentRenderer* r = static_cast<FragmentRenderer*>(renderer);
    fragmentForgetResources(r->fragment);
    fragmentDeviceDropResources(r->state);
}

void stats(const PrDrawFrame* handle, PrDrawStats* out) {
    const FragmentDrawFrame* frame = static_cast<const FragmentDrawFrame*>(handle);
    const FragmentFrame& f = frame->built;
    // Counts one draw for each run of a pass's instances that either all read the destination or all do not, which is
    // how a device whose reads of the target are ordered draws them.
    out->passes = 1;
    for (uint32_t i = 0; i < f.passCount; ++i) {
        const FragmentPass& pass = f.passes[i];
        out->passes += pass.layer != 0 ? 1 : 0;
        bool reads = false;
        for (uint32_t k = 0; k < pass.count; ++k) {
            bool now = (f.instances[pass.first + k].flags & FragmentInstanceFlag_ReadsDestination) != 0;
            out->draws += k == 0 || now != reads ? 1 : 0;
            reads = now;
        }
    }
    // The beneath layer is the host's texture, which no pass draws into.
    out->layers = f.layerCount - (f.beneath != 0 ? 1 : 0);
    out->primitives = f.instanceCount;
    out->records = f.paintCount + f.clipCount;
    out->pathFills = frame->storage->pathFills;
    uint64_t bytes = uint64_t(f.instanceCount) * sizeof(FragmentInstance) + uint64_t(f.valueCount) * 4 +
        f.coverageCount + uint64_t(f.tileDataCount) * 4 + uint64_t(f.paintCount) * sizeof(FragmentPaint) +
        uint64_t(f.clipCount) * sizeof(FragmentClip) + uint64_t(f.clipShapeCount) * 4 +
        uint64_t(f.shapeCount) * sizeof(FragmentClipShape) + uint64_t(f.pieceCount) * sizeof(FragmentPiece) +
        uint64_t(f.mapCount) * sizeof(FragmentMap) + uint64_t(f.mapDataCount) * 4 + uint64_t(f.mapIndexCount) * 4 +
        uint64_t(f.rampCount) * (kFragmentRampResolution + 1) * sizeof(PrColor16) +
        uint64_t(f.rampCount) * (kFragmentRampResolution + 2) * 4 +
        uint64_t(f.rampPieceCount) * sizeof(PrDrawRampPiece) + uint64_t(f.rampColorCount) * sizeof(PrColor16) +
        uint64_t(f.textureCount) * sizeof(FragmentTexture) +
        uint64_t(f.filterCount) * sizeof(FragmentFilter) + uint64_t(f.filterDataCount) * 4;
    for (uint32_t i = 0; i < f.requestCount; ++i) {
        const FragmentTextureRequest& r = f.requests[i];
        if (r.op != FragmentTextureOp_Destroy)
            bytes += uint64_t(r.texels.extent.x) * r.texels.extent.y * prPixelFormatSize(r.format);
    }
    out->uploadBytes = bytes;
}

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    FragmentDrawFrame* frame = allocZeroed<FragmentDrawFrame>();
    frame->destroyFn = destroyFrame;
    frame->prepareFn = prepare;
    frame->renderFn = render;
    frame->statsFn = stats;
    frame->renderer = static_cast<FragmentRenderer*>(renderer);
    frame->storage = allocZeroed<FrameStorage>();
    return frame;
}

void destroyRenderer(PrDrawRenderer* renderer) {
    FragmentRenderer* r = static_cast<FragmentRenderer*>(renderer);
    fragmentDestroy(r->fragment);
    fragmentDeviceDestroy(r->state);
    prMemoryFree(r);
}

FragmentRenderer* createRenderer(FragmentDevice* device, const PrDrawFragmentOptions* PR_NULLABLE options) {
    FragmentRenderer* r = allocZeroed<FragmentRenderer>();
    r->state = device;
    r->fragment = fragmentCreate(options != nullptr ? *options : prDrawFragmentOptionsDefault());
    r->destroyFn = destroyRenderer;
    r->frameCreateFn = createFrame;
    r->deviceFn = rendererDevice;
    r->dropResourcesFn = dropResources;
    return r;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawFragmentOptions prDrawFragmentOptionsDefault(void) {
    PrDrawFragmentOptions options{};
    options.paths = PrDrawFragmentPaths_Tiles;
    options.atlasSize = 1024;
    return options;
}

PrStatus prDrawFragmentCreate(PrGpuDevice* device, const PrDrawFragmentOptions* PR_NULLABLE options,
        PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    FragmentDevice* state = nullptr;
    PrStatus status = fragmentDeviceCreate(device, &state);
    if (status != PrStatus_Ok)
        return status;
    FragmentRenderer* renderer = createRenderer(state, options);
    renderer->device = device;
    *out = renderer;
    return PrStatus_Ok;
}
